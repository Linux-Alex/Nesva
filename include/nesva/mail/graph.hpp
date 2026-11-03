// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <functional>
#include <memory>

#include "nesva/core/result.hpp"
#include "nesva/mail/http.hpp"
#include "nesva/mail/oauth.hpp"

namespace nesva::mail {

inline constexpr const char* kGraphBaseUrl = "https://graph.microsoft.com/v1.0";

/// Supplies access tokens, refreshing them when they expire and persisting a
/// rotated refresh token through the callback.
class TokenProvider {
public:
    TokenProvider(OAuthConfig config, QString refreshToken,
                  std::shared_ptr<net::HttpTransport> transport);

    Result<QString> accessToken();
    void invalidateAccessToken();
    const QString& refreshToken() const { return refreshToken_; }
    void setRefreshToken(QString token) { refreshToken_ = std::move(token); }
    const OAuthConfig& config() const { return config_; }

    /// Called whenever the service issues a new refresh token.
    std::function<void(const QString&)> onRefreshTokenChanged;

private:
    OAuthConfig config_;
    QString refreshToken_;
    std::shared_ptr<net::HttpTransport> transport_;
    QString accessToken_;
    QDateTime accessExpiryUtc_;
};

struct MailFolderInfo {
    QString id;
    QString displayName;
    QString parentId;
    int childFolderCount = 0;
    qint64 totalItemCount = 0;
    bool hidden = false;
    /// Full path (parent/child) built while walking the tree.
    QString path;
};

struct MailMessageInfo {
    QString id;
    QString internetMessageId;
    QString subject;
    QString sender;
    QDateTime receivedUtc;
    QDateTime lastModifiedUtc;
    QString parentFolderId;
    bool hasAttachments = false;
    qint64 sizeBytes = 0;
    bool removed = false;
};

struct DeltaPage {
    QVector<MailMessageInfo> messages;
    QString nextLink;
    QString deltaLink;
};

/// Thin, testable client for the Microsoft Graph mail endpoints Nesva needs.
///
/// Only read operations are implemented: nothing here can modify, move or
/// delete mail in Microsoft 365.
class GraphClient {
public:
    GraphClient(std::shared_ptr<TokenProvider> tokens,
                std::shared_ptr<net::HttpTransport> transport = nullptr);

    /// Validates the session and returns the signed-in user profile.
    Result<QJsonObject> me();

    /// All mail folders, flattened, with parent/child paths.
    Result<QVector<MailFolderInfo>> mailFolders();

    /// One page of the message delta feed for a folder. Pass an empty
    /// \p nextOrDeltaUrl to start from the folder's current delta cursor.
    Result<DeltaPage> messagesDelta(const QString& folderId, const QString& nextOrDeltaUrl);

    /// Raw RFC822 source of one message (the MIME content endpoint).
    Result<QByteArray> messageMime(const QString& messageId);

    // Parsers, exposed for tests.
    static Result<QVector<MailMessageInfo>> parseMessages(const QJsonArray& array);
    static Result<QVector<MailFolderInfo>> parseFolders(const QJsonArray& array,
                                                        const QString& parentPath = {},
                                                        const QString& parentId = {});

private:
    Result<net::HttpResponse> authorizedSend(net::HttpVerb verb, const QUrl& url,
                                             const QHash<QString, QString>& headers,
                                             const QByteArray& body);
    Result<QJsonObject> getJson(const QUrl& url);

    std::shared_ptr<TokenProvider> tokens_;
    std::shared_ptr<net::HttpTransport> transport_;
};

} // namespace nesva::mail
