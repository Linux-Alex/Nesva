// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/mail/graph.hpp"

#include <QtCore/QJsonDocument>
#include <QtCore/QThread>
#include <QtCore/QUrlQuery>

#include <utility>

#include "nesva/core/logging.hpp"

namespace nesva::mail {
namespace {

constexpr int kRequestTimeoutMs = 60000;
constexpr int kMaxAttempts = 4;

QDateTime parseGraphTime(const QString& value) {
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed.isValid() ? parsed.toUTC() : QDateTime();
}

QString senderOf(const QJsonObject& message) {
    const QJsonObject from = message.value(QStringLiteral("from")).toObject();
    const QJsonObject address = from.value(QStringLiteral("emailAddress")).toObject();
    const QString name = address.value(QStringLiteral("name")).toString();
    const QString mail = address.value(QStringLiteral("address")).toString();
    if (name.isEmpty()) {
        return mail;
    }
    return mail.isEmpty() ? name : QStringLiteral("%1 <%2>").arg(name, mail);
}

int retryDelaySeconds(const net::HttpResponse& response, int attempt) {
    const QString retryAfter = response.headers.value(QStringLiteral("retry-after"));
    bool ok = false;
    const int seconds = retryAfter.toInt(&ok);
    if (ok && seconds > 0) {
        return qMin(seconds, 120);
    }
    return qMin(2 * (attempt + 1), 30);
}

} // namespace

TokenProvider::TokenProvider(OAuthConfig config, QString refreshToken,
                             std::shared_ptr<net::HttpTransport> transport)
    : config_(std::move(config)),
      refreshToken_(std::move(refreshToken)),
      transport_(transport ? std::move(transport) : std::make_shared<net::QNetworkHttpTransport>()) {}

Result<QString> TokenProvider::accessToken() {
    if (!accessToken_.isEmpty() &&
        (!accessExpiryUtc_.isValid() ||
         QDateTime::currentDateTimeUtc().addSecs(60) < accessExpiryUtc_)) {
        return accessToken_;
    }

    const Result<TokenSet> refreshed = refreshTokens(config_, refreshToken_, transport_);
    if (!refreshed) {
        return refreshed.error();
    }
    accessToken_ = refreshed.value().accessToken;
    accessExpiryUtc_ = refreshed.value().expiresAtUtc;
    if (!refreshed.value().refreshToken.isEmpty() &&
        refreshed.value().refreshToken != refreshToken_) {
        refreshToken_ = refreshed.value().refreshToken;
        if (onRefreshTokenChanged) {
            onRefreshTokenChanged(refreshToken_);
        }
    }
    return accessToken_;
}

void TokenProvider::invalidateAccessToken() {
    accessToken_.clear();
    accessExpiryUtc_ = QDateTime();
}

GraphClient::GraphClient(std::shared_ptr<TokenProvider> tokens,
                         std::shared_ptr<net::HttpTransport> transport)
    : tokens_(std::move(tokens)),
      transport_(transport ? std::move(transport) : std::make_shared<net::QNetworkHttpTransport>()) {}

Result<net::HttpResponse> GraphClient::authorizedSend(net::HttpVerb verb, const QUrl& url,
                                                      const QHash<QString, QString>& headers,
                                                      const QByteArray& body) {
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const Result<QString> token = tokens_->accessToken();
        if (!token) {
            return token.error();
        }
        QHash<QString, QString> requestHeaders = headers;
        requestHeaders.insert(QStringLiteral("Authorization"),
                              QStringLiteral("Bearer ") + token.value());

        const net::HttpResponse response =
            transport_->send(verb, url, requestHeaders, body, kRequestTimeoutMs);
        if (response.isSuccess()) {
            return response;
        }
        if (response.isAuthFailure()) {
            // The token was revoked or expired early: refresh once, then fail.
            tokens_->invalidateAccessToken();
            if (attempt >= 1) {
                return makeErrorValue(ErrorCode::PermissionDenied,
                                      "Microsoft Graph rejected the access token",
                                      response.body.left(300).toStdString());
            }
            continue;
        }
        if (response.isRateLimited() || response.statusCode >= 500) {
            const int delay = retryDelaySeconds(response, attempt);
            logWarn("mail.graph", QStringLiteral("throttled or server error; retrying"),
                    QJsonObject{{QStringLiteral("status"), response.statusCode},
                                {QStringLiteral("delay_seconds"), delay}});
            QThread::msleep(static_cast<unsigned long>(delay) * 1000UL);
            continue;
        }
        if (response.statusCode == 0) {
            return makeErrorValue(ErrorCode::ExternalToolFailed, "cannot reach Microsoft Graph",
                                  response.error.toStdString());
        }
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "Microsoft Graph request failed",
                              QString::number(response.statusCode).toStdString() + ": " +
                                  response.body.left(300).toStdString());
    }
    return makeErrorValue(ErrorCode::Timeout, "Microsoft Graph request kept failing");
}

Result<QJsonObject> GraphClient::getJson(const QUrl& url) {
    const Result<net::HttpResponse> response = authorizedSend(
        net::HttpVerb::Get, url,
        {{QStringLiteral("Accept"), QStringLiteral("application/json")}}, {});
    if (!response) {
        return response.error();
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(response.value().body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "Microsoft Graph returned an unreadable response",
                              parseError.errorString().toStdString());
    }
    return document.object();
}

Result<QJsonObject> GraphClient::me() {
    return getJson(QUrl(QStringLiteral("%1/me").arg(QString::fromUtf8(kGraphBaseUrl))));
}

Result<QVector<MailFolderInfo>> GraphClient::parseFolders(const QJsonArray& array,
                                                          const QString& parentPath,
                                                          const QString& parentId) {
    QVector<MailFolderInfo> folders;
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        MailFolderInfo folder;
        folder.id = object.value(QStringLiteral("id")).toString();
        folder.displayName = object.value(QStringLiteral("displayName")).toString();
        folder.parentId = parentId;
        folder.childFolderCount = object.value(QStringLiteral("childFolderCount")).toInt(0);
        folder.totalItemCount =
            static_cast<qint64>(object.value(QStringLiteral("totalItemCount")).toDouble(0));
        folder.hidden = object.value(QStringLiteral("isHidden")).toBool(false);
        if (folder.id.isEmpty()) {
            continue;
        }
        folder.path = parentPath.isEmpty()
                          ? folder.displayName
                          : parentPath + QLatin1Char('/') + folder.displayName;
        folders.append(folder);
    }
    return folders;
}

Result<QVector<MailFolderInfo>> GraphClient::mailFolders() {
    QUrl url(QStringLiteral("%1/me/mailFolders").arg(QString::fromUtf8(kGraphBaseUrl)));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("$top"), QStringLiteral("100"));
    query.addQueryItem(QStringLiteral("$select"),
                       QStringLiteral("id,displayName,parentFolderId,childFolderCount,"
                                      "totalItemCount,isHidden"));
    url.setQuery(query);

    QVector<MailFolderInfo> collected;
    QVector<MailFolderInfo> pending = {};

    const Result<QJsonObject> root = getJson(url);
    if (!root) {
        return root.error();
    }
    const Result<QVector<MailFolderInfo>> rootFolders =
        parseFolders(root.value().value(QStringLiteral("value")).toArray());
    pending.append(rootFolders.value());

    while (!pending.isEmpty()) {
        const MailFolderInfo folder = pending.takeFirst();
        collected.append(folder);
        if (folder.childFolderCount <= 0) {
            continue;
        }
        QUrl childrenUrl(QStringLiteral("%1/me/mailFolders/%2/childFolders")
                             .arg(QString::fromUtf8(kGraphBaseUrl),
                                  QString::fromUtf8(folder.id.toUtf8())));
        QUrlQuery childrenQuery;
        childrenQuery.addQueryItem(QStringLiteral("$top"), QStringLiteral("100"));
        childrenQuery.addQueryItem(QStringLiteral("$select"),
                                   QStringLiteral("id,displayName,parentFolderId,childFolderCount,"
                                                  "totalItemCount,isHidden"));
        childrenUrl.setQuery(childrenQuery);

        const Result<QJsonObject> children = getJson(childrenUrl);
        if (!children) {
            // A folder we cannot enumerate must not abort the whole listing.
            logWarn("mail.graph", QStringLiteral("cannot list child folders"),
                    QJsonObject{{QStringLiteral("folder"), folder.displayName}});
            continue;
        }
        const Result<QVector<MailFolderInfo>> parsed = parseFolders(
            children.value().value(QStringLiteral("value")).toArray(), folder.path, folder.id);
        for (const MailFolderInfo& child : parsed.value()) {
            pending.append(child);
        }
    }
    return collected;
}

Result<QVector<MailMessageInfo>> GraphClient::parseMessages(const QJsonArray& array) {
    QVector<MailMessageInfo> messages;
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        MailMessageInfo message;
        message.id = object.value(QStringLiteral("id")).toString();
        if (message.id.isEmpty()) {
            continue;
        }
        message.removed = object.contains(QStringLiteral("@removed"));
        message.internetMessageId = object.value(QStringLiteral("internetMessageId")).toString();
        message.subject = object.value(QStringLiteral("subject")).toString();
        message.sender = senderOf(object);
        message.receivedUtc = parseGraphTime(object.value(QStringLiteral("receivedDateTime")).toString());
        message.lastModifiedUtc =
            parseGraphTime(object.value(QStringLiteral("lastModifiedDateTime")).toString());
        message.parentFolderId = object.value(QStringLiteral("parentFolderId")).toString();
        message.hasAttachments = object.value(QStringLiteral("hasAttachments")).toBool(false);
        message.sizeBytes =
            static_cast<qint64>(object.value(QStringLiteral("size")).toDouble(0));
        messages.append(message);
    }
    return messages;
}

Result<DeltaPage> GraphClient::messagesDelta(const QString& folderId,
                                             const QString& nextOrDeltaUrl) {
    QUrl url;
    if (!nextOrDeltaUrl.isEmpty()) {
        url = QUrl(nextOrDeltaUrl);
    } else {
        url = QUrl(QStringLiteral("%1/me/mailFolders/%2/messages/delta")
                       .arg(QString::fromUtf8(kGraphBaseUrl),
                            QString::fromUtf8(folderId.toUtf8())));
        QUrlQuery query;
        // Delta supports only a subset of message properties. Asking for "size"
        // makes Exchange reject the whole request with RequestBroker--ParseUri
        // ("Could not find a property named 'size' on type 'microsoft.graph.message'").
        // The archived size is the byte count of the downloaded .eml instead.
        query.addQueryItem(QStringLiteral("$select"),
                           QStringLiteral("id,internetMessageId,subject,from,receivedDateTime,"
                                          "lastModifiedDateTime,parentFolderId,hasAttachments"));
        url.setQuery(query);
    }

    const Result<QJsonObject> page = getJson(url);
    if (!page) {
        return page.error();
    }
    DeltaPage delta;
    const Result<QVector<MailMessageInfo>> messages =
        parseMessages(page.value().value(QStringLiteral("value")).toArray());
    if (!messages) {
        return messages.error();
    }
    delta.messages = messages.value();
    delta.nextLink = page.value().value(QStringLiteral("@odata.nextLink")).toString();
    delta.deltaLink = page.value().value(QStringLiteral("@odata.deltaLink")).toString();
    return delta;
}

Result<QByteArray> GraphClient::messageMime(const QString& messageId) {
    const QUrl url(QStringLiteral("%1/me/messages/%2/$value")
                       .arg(QString::fromUtf8(kGraphBaseUrl),
                            QString::fromUtf8(messageId.toUtf8())));
    const Result<net::HttpResponse> response = authorizedSend(
        net::HttpVerb::Get, url,
        {{QStringLiteral("Accept"), QStringLiteral("message/rfc822")}}, {});
    if (!response) {
        return response.error();
    }
    if (response.value().body.isEmpty()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "Microsoft Graph returned an empty message body");
    }
    return response.value().body;
}

} // namespace nesva::mail
