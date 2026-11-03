// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/mail/oauth.hpp"

#include <QtCore/QCryptographicHash>
#include <QtCore/QEventLoop>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <utility>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <shellapi.h>
#else
#include <QtCore/QProcess>
#endif

#include "nesva/core/logging.hpp"

namespace nesva::mail {
namespace {

constexpr const char* kSuccessPage =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>Nesva</title></head>"
    "<body style=\"font-family:sans-serif;margin:3rem\"><h2>Nesva is connected</h2>"
    "<p>You can close this window and return to Nesva.</p></body></html>";

constexpr const char* kFailurePage =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>Nesva</title></head>"
    "<body style=\"font-family:sans-serif;margin:3rem\"><h2>Sign-in failed</h2>"
    "<p>%1</p><p>You can close this window and try again.</p></body></html>";

QString randomState() {
    QByteArray bytes(16, 0);
    QRandomGenerator::system()->fillRange(
        reinterpret_cast<quint32*>(bytes.data()), static_cast<qsizetype>(bytes.size() / 4));
    return base64UrlEncode(bytes);
}

/// Extracts a query parameter from a raw request target ("/callback?code=..").
QString queryParameter(const QByteArray& request, const QByteArray& name) {
    const int lineEnd = request.indexOf("\r\n");
    const QByteArray requestLine = lineEnd > 0 ? request.left(lineEnd) : request;
    const int spaceAfterMethod = requestLine.indexOf(' ');
    const int spaceAfterTarget = requestLine.indexOf(' ', spaceAfterMethod + 1);
    if (spaceAfterMethod < 0 || spaceAfterTarget < 0) {
        return {};
    }
    const QByteArray target = requestLine.mid(spaceAfterMethod + 1,
                                              spaceAfterTarget - spaceAfterMethod - 1);
    const int question = target.indexOf('?');
    if (question < 0) {
        return {};
    }
    const QUrlQuery query(QString::fromUtf8(target.mid(question + 1)));
    return query.queryItemValue(QString::fromUtf8(name), QUrl::FullyDecoded);
}

} // namespace

// ---------------------------------------------------------------------------
// PKCE
// ---------------------------------------------------------------------------

QString base64UrlEncode(const QByteArray& data) {
    return QString::fromLatin1(
        data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString computePkceChallenge(const QString& verifier) {
    return base64UrlEncode(
        QCryptographicHash::hash(verifier.toUtf8(), QCryptographicHash::Sha256));
}

PkcePair generatePkce() {
    QByteArray random(32, 0);
    QRandomGenerator::system()->fillRange(
        reinterpret_cast<quint32*>(random.data()), static_cast<qsizetype>(random.size() / 4));
    PkcePair pair;
    pair.verifier = base64UrlEncode(random);
    pair.challenge = computePkceChallenge(pair.verifier);
    return pair;
}

bool TokenSet::needsRefresh() const {
    if (accessToken.isEmpty()) {
        return true;
    }
    if (!expiresAtUtc.isValid()) {
        return false;
    }
    return QDateTime::currentDateTimeUtc().addSecs(60) >= expiresAtUtc;
}

// ---------------------------------------------------------------------------
// Loopback redirect
// ---------------------------------------------------------------------------

LoopbackServer::LoopbackServer() = default;

LoopbackServer::~LoopbackServer() {
    if (server4_) {
        server4_->close();
    }
    if (server6_) {
        server6_->close();
    }
}

Status LoopbackServer::listen() {
    server4_ = std::make_unique<QTcpServer>();
    if (!server4_->listen(QHostAddress::LocalHost, 0)) {
        return makeError(ErrorCode::IoError, "cannot open the loopback redirect listener",
                         server4_->errorString().toStdString());
    }
    port_ = server4_->serverPort();

    // Browsers resolve "localhost" to either family; bind both when possible.
    server6_ = std::make_unique<QTcpServer>();
    if (!server6_->listen(QHostAddress::LocalHostIPv6, port_)) {
        server6_.reset();
    }
    return Status();
}

QString LoopbackServer::redirectUri() const {
    // Registered as "http://localhost": Microsoft ignores the port for loopback
    // URIs, so the root path is used to keep the match exact.
    return QStringLiteral("http://localhost:%1/").arg(port_);
}

Result<QString> LoopbackServer::waitForCode(const QString& expectedState, int timeoutMs) {
    if (!server4_) {
        return makeErrorValue(ErrorCode::Internal, "the loopback listener is not running");
    }

    QEventLoop loop;
    QString code;
    QString failure;
    bool finished = false;

    const auto handle = [&](QTcpServer* server) {
        QTcpSocket* socket = server->nextPendingConnection();
        if (socket == nullptr) {
            return;
        }
        socket->waitForReadyRead(5000);
        const QByteArray request = socket->readAll();

        const QString receivedState = queryParameter(request, "state");
        const QString receivedCode = queryParameter(request, "code");
        const QString receivedError = queryParameter(request, "error");
        const QString receivedDescription = queryParameter(request, "error_description");

        QString page = kSuccessPage;
        if (!receivedError.isEmpty()) {
            failure = receivedError + QStringLiteral(": ") + receivedDescription;
            page = QString::fromUtf8(kFailurePage).arg(failure.toHtmlEscaped());
        } else if (receivedState != expectedState) {
            failure = QStringLiteral("the redirect state did not match (possible CSRF)");
            page = QString::fromUtf8(kFailurePage).arg(failure.toHtmlEscaped());
        } else if (receivedCode.isEmpty()) {
            failure = QStringLiteral("the redirect contained no authorization code");
            page = QString::fromUtf8(kFailurePage).arg(failure.toHtmlEscaped());
        } else {
            code = receivedCode;
        }

        const QByteArray body = page.toUtf8();
        const QByteArray response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
            QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
        socket->deleteLater();

        finished = true;
        loop.quit();
    };

    QObject::connect(server4_.get(), &QTcpServer::newConnection, &loop,
                     [&] { handle(server4_.get()); });
    if (server6_) {
        QObject::connect(server6_.get(), &QTcpServer::newConnection, &loop,
                         [&] { handle(server6_.get()); });
    }

    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    if (!finished) {
        return makeErrorValue(ErrorCode::Timeout,
                              "the sign-in window did not complete in time");
    }
    if (!failure.isEmpty()) {
        return makeErrorValue(ErrorCode::PermissionDenied, "sign-in failed", failure.toStdString());
    }
    return code;
}

// ---------------------------------------------------------------------------
// URLs and request bodies
// ---------------------------------------------------------------------------

QString buildAuthorizeUrl(const OAuthConfig& config, const QString& redirectUri,
                          const PkcePair& pkce, const QString& state) {
    QUrl url(QStringLiteral("%1/%2/oauth2/v2.0/authorize")
                 .arg(config.authorityHost, config.tenantId));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_id"), config.clientId);
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("redirect_uri"), redirectUri);
    query.addQueryItem(QStringLiteral("response_mode"), QStringLiteral("query"));
    query.addQueryItem(QStringLiteral("scope"), config.scopes.join(QLatin1Char(' ')));
    query.addQueryItem(QStringLiteral("state"), state);
    query.addQueryItem(QStringLiteral("code_challenge"), pkce.challenge);
    query.addQueryItem(QStringLiteral("code_challenge_method"), pkce.method);
    query.addQueryItem(QStringLiteral("prompt"), QStringLiteral("select_account"));
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

QByteArray buildTokenRequestBody(const OAuthConfig& config, const QString& redirectUri,
                                 const QString& code, const PkcePair& pkce) {
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("client_id"), config.clientId);
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    form.addQueryItem(QStringLiteral("code"), code);
    form.addQueryItem(QStringLiteral("redirect_uri"), redirectUri);
    form.addQueryItem(QStringLiteral("code_verifier"), pkce.verifier);
    form.addQueryItem(QStringLiteral("scope"), config.scopes.join(QLatin1Char(' ')));
    return form.toString(QUrl::FullyEncoded).toUtf8();
}

QByteArray buildRefreshRequestBody(const OAuthConfig& config, const QString& refreshToken) {
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("client_id"), config.clientId);
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    form.addQueryItem(QStringLiteral("refresh_token"), refreshToken);
    form.addQueryItem(QStringLiteral("scope"), config.scopes.join(QLatin1Char(' ')));
    return form.toString(QUrl::FullyEncoded).toUtf8();
}

QString aadFixHint(const QString& errorResponseBody) {
    // Ordered from the most specific configuration mistakes to the generic
    // ones: the first code found in the body wins.
    struct Hint {
        const char* code;
        const char* text;
    };
    static constexpr Hint kHints[] = {
        {"AADSTS7000218",
         "Entra treats this app registration as a confidential client. In the app registration open "
         "Authentication, set 'Allow public client flows' to Yes, and keep the loopback redirect "
         "under the 'Mobile and desktop applications' platform (never under 'Web'). Nesva is a "
         "public client on purpose and never uses a client secret."},
        {"AADSTS50011",
         "The redirect URI is not registered for this app. Add http://localhost under "
         "'Mobile and desktop applications' in the app registration's Authentication page."},
        {"AADSTS900971",
         "The app registration has no reply address. Add http://localhost under 'Mobile and "
         "desktop applications' in the Authentication page."},
        {"AADSTS700016",
         "The client id is not registered in this tenant. Check --client and --tenant."},
        {"AADSTS65001",
         "Consent for Mail.Read is missing. Grant tenant-wide admin consent for the app, or "
         "consent again interactively."},
        {"AADSTS50105",
         "The signed-in account is not assigned to this application. Assign the account under "
         "Enterprise applications, or turn 'Assignment required' off."},
        {"AADSTS70008",
         "The authorization code expired. Run the sign-in again."},
        {"AADSTS54005",
         "The authorization code was already redeemed. Run the sign-in again."},
        {"AADSTS7000215",
         "The client sent an invalid secret. Nesva must never send one: remove the secret and "
         "enable 'Allow public client flows'."},
        {"AADSTS7000222",
         "The client secret expired. Nesva must never need a secret: enable 'Allow public client "
         "flows' and do not configure one."},
        {"AADSTS9002326",
         "The app registration is configured as a single-page application. Register the loopback "
         "redirect under 'Mobile and desktop applications' instead."},
    };

    for (const Hint& hint : kHints) {
        if (errorResponseBody.contains(QLatin1String(hint.code))) {
            return QString::fromUtf8(hint.text);
        }
    }
    return {};
}

Result<TokenSet> parseTokenResponse(const QByteArray& body) {
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "the identity platform returned an unreadable response",
                              parseError.errorString().toStdString());
    }
    const QJsonObject object = document.object();

    const QString error = object.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
        const QString description = object.value(QStringLiteral("error_description")).toString();
        return makeErrorValue(ErrorCode::PermissionDenied,
                              "the identity platform refused the token request",
                              (error + QStringLiteral(": ") + description).toStdString());
    }

    TokenSet tokens;
    tokens.accessToken = object.value(QStringLiteral("access_token")).toString();
    tokens.refreshToken = object.value(QStringLiteral("refresh_token")).toString();
    tokens.idToken = object.value(QStringLiteral("id_token")).toString();
    tokens.tokenType = object.value(QStringLiteral("token_type")).toString();
    tokens.scope = object.value(QStringLiteral("scope")).toString();
    const qint64 expiresIn = static_cast<qint64>(
        object.value(QStringLiteral("expires_in")).toDouble(3600));
    tokens.expiresAtUtc = QDateTime::currentDateTimeUtc().addSecs(expiresIn);

    if (!tokens.isValid()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "the identity platform returned no access token");
    }
    return tokens;
}

bool openBrowser(const QString& url) {
#if defined(Q_OS_WIN)
    const std::wstring wide = url.toStdWString();
    const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr,
                                             SW_SHOWNORMAL);
    return reinterpret_cast<qintptr>(result) > 32;
#elif defined(Q_OS_MACOS)
    return QProcess::startDetached(QStringLiteral("open"), {url});
#else
    return QProcess::startDetached(QStringLiteral("xdg-open"), {url});
#endif
}

Result<TokenSet> interactiveAuthorize(const OAuthConfig& config,
                                      const std::shared_ptr<net::HttpTransport>& transport,
                                      int timeoutMs, const std::function<void(const QString&)>& onUrl) {
    if (config.tenantId.isEmpty() || config.clientId.isEmpty()) {
        return makeErrorValue(ErrorCode::ConfigurationError,
                              "the Microsoft 365 tenant and client id must be configured");
    }

    LoopbackServer server;
    const Status listening = server.listen();
    if (!listening) {
        return listening.error();
    }

    const PkcePair pkce = generatePkce();
    const QString state = randomState();
    const QString redirectUri = server.redirectUri();
    const QString authorizeUrl = buildAuthorizeUrl(config, redirectUri, pkce, state);

    if (onUrl) {
        onUrl(authorizeUrl);
    }
    if (!openBrowser(authorizeUrl)) {
        logWarn("mail.oauth", QStringLiteral("could not open a browser automatically"));
    }

    const Result<QString> code = server.waitForCode(state, timeoutMs);
    if (!code) {
        return code.error();
    }

    const QUrl tokenUrl(
        QStringLiteral("%1/%2/oauth2/v2.0/token").arg(config.authorityHost, config.tenantId));
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Content-Type"),
                   QStringLiteral("application/x-www-form-urlencoded"));

    net::HttpResponse response = transport->send(net::HttpVerb::Post, tokenUrl, headers,
                                                 buildTokenRequestBody(config, redirectUri,
                                                                       code.value(), pkce),
                                                 timeoutMs);

    // Some tenants match the registered loopback URI without the port; retry
    // once with the bare form if the platform reports a redirect mismatch.
    if (response.body.contains("AADSTS50011")) {
        const QString bareRedirect = QStringLiteral("http://localhost");
        response = transport->send(net::HttpVerb::Post, tokenUrl, headers,
                                   buildTokenRequestBody(config, bareRedirect, code.value(), pkce),
                                   timeoutMs);
    }

    if (response.statusCode < 200 || response.statusCode >= 300) {
        if (response.statusCode == 0) {
            // No HTTP exchange happened at all: report the transport failure
            // rather than a status code, which would read as "(0: )".
            return makeErrorValue(ErrorCode::ExternalToolFailed,
                                  "the token endpoint could not be reached",
                                  response.error.isEmpty()
                                      ? "the request failed without an HTTP response"
                                      : response.error.toStdString());
        }
        const QJsonObject error = QJsonDocument::fromJson(response.body).object();
        const QString description =
            error.value(QStringLiteral("error_description")).toString();
        const QString raw = QString::number(response.statusCode) + QStringLiteral(": ") +
                            (description.isEmpty() ? QString::fromUtf8(response.body.left(300))
                                                   : description);
        logWarn("mail.oauth", QStringLiteral("the token request was rejected"),
                QJsonObject{{QStringLiteral("status"), response.statusCode}});
        // Lead with the fix: the raw AADSTS text is unreadable on its own, but
        // it carries the trace id support needs, so it is kept after it.
        const QString hint = aadFixHint(QString::fromUtf8(response.body));
        const QString detail =
            hint.isEmpty() ? raw : hint + QStringLiteral(" Entra reported ") + raw;
        return makeErrorValue(ErrorCode::PermissionDenied, "the token request failed",
                              detail.toStdString());
    }
    return parseTokenResponse(response.body);
}

Result<TokenSet> refreshTokens(const OAuthConfig& config, const QString& refreshToken,
                               const std::shared_ptr<net::HttpTransport>& transport) {
    if (refreshToken.isEmpty()) {
        return makeErrorValue(ErrorCode::PermissionDenied, "no refresh token is stored",
                              "connect the Microsoft 365 account again");
    }
    const QUrl tokenUrl(
        QStringLiteral("%1/%2/oauth2/v2.0/token").arg(config.authorityHost, config.tenantId));
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Content-Type"),
                   QStringLiteral("application/x-www-form-urlencoded"));

    const net::HttpResponse response = transport->send(
        net::HttpVerb::Post, tokenUrl, headers, buildRefreshRequestBody(config, refreshToken),
        30000);
    if (response.statusCode < 200 || response.statusCode >= 300) {
        if (response.statusCode == 0) {
            return makeErrorValue(ErrorCode::ExternalToolFailed,
                                  "the token endpoint could not be reached",
                                  response.error.toStdString());
        }
        const QJsonObject error = QJsonDocument::fromJson(response.body).object();
        const QString description =
            error.value(QStringLiteral("error_description")).toString();
        const QString hint = aadFixHint(QString::fromUtf8(response.body));
        QString detail = description;
        if (detail.isEmpty()) {
            detail = QString::fromUtf8(response.body.left(300));
        }
        if (!hint.isEmpty()) {
            detail = hint + QStringLiteral(" Entra reported ") + detail;
        }
        return makeErrorValue(ErrorCode::PermissionDenied, "the refresh token was rejected",
                              detail.toStdString());
    }
    return parseTokenResponse(response.body);
}

} // namespace nesva::mail
