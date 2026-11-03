// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>
#include <memory>
#include <optional>

#include "nesva/core/result.hpp"
#include "nesva/mail/http.hpp"

class QTcpServer;
class QTcpSocket;

namespace nesva::mail {

/// PKCE pair (RFC 7636). The verifier never leaves the process; only its
/// SHA-256 challenge goes to the authorization endpoint.
struct PkcePair {
    QString verifier;
    QString challenge;
    QString method = QStringLiteral("S256");
};

PkcePair generatePkce();

/// base64url without padding, as required by PKCE and JWT.
QString base64UrlEncode(const QByteArray& data);

/// The challenge for a given verifier (RFC 7636 S256). Exposed for tests.
QString computePkceChallenge(const QString& verifier);

struct OAuthConfig {
    QString tenantId;
    QString clientId;
    QStringList scopes = {QStringLiteral("openid"), QStringLiteral("profile"),
                          QStringLiteral("offline_access"), QStringLiteral("Mail.Read"),
                          QStringLiteral("User.Read")};
    QString authorityHost = QStringLiteral("https://login.microsoftonline.com");
};

/// Loopback redirect listener (RFC 8252).
///
/// Binds ephemeral ports on 127.0.0.1 and ::1 so a browser that resolves
/// "localhost" to either family reaches us. The registered redirect URI is
/// http://localhost; Microsoft ignores the port for loopback URIs.
class LoopbackServer {
public:
    LoopbackServer();
    ~LoopbackServer();

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    Status listen();
    quint16 port() const { return port_; }
    QString redirectUri() const;

    /// Waits for the browser redirect and returns the authorization code.
    /// Rejects a mismatched state (CSRF protection) and surfaces AAD error
    /// responses as errors instead of hanging.
    Result<QString> waitForCode(const QString& expectedState, int timeoutMs);

private:
    std::unique_ptr<QTcpServer> server4_;
    std::unique_ptr<QTcpServer> server6_;
    quint16 port_ = 0;
};

struct TokenSet {
    QString accessToken;
    QString refreshToken;
    QString idToken;
    QDateTime expiresAtUtc;
    QString scope;
    QString tokenType;

    bool isValid() const { return !accessToken.isEmpty(); }
    bool needsRefresh() const;
};

QString buildAuthorizeUrl(const OAuthConfig& config, const QString& redirectUri,
                          const PkcePair& pkce, const QString& state);
QByteArray buildTokenRequestBody(const OAuthConfig& config, const QString& redirectUri,
                                 const QString& code, const PkcePair& pkce);
QByteArray buildRefreshRequestBody(const OAuthConfig& config, const QString& refreshToken);

/// Parses a v2.0 token endpoint response (success or error).
Result<TokenSet> parseTokenResponse(const QByteArray& body);

/// Opens the system browser. Returns false when no handler is available.
bool openBrowser(const QString& url);

/// Ends the interactive flow: builds the URL, starts the loopback listener,
/// opens the browser and exchanges the code for tokens.
Result<TokenSet> interactiveAuthorize(const OAuthConfig& config,
                                      const std::shared_ptr<net::HttpTransport>& transport,
                                      int timeoutMs = 300000,
                                      const std::function<void(const QString&)>& onUrl = {});

/// Exchanges a refresh token. A rotated refresh token is returned when the
/// service issues one.
Result<TokenSet> refreshTokens(const OAuthConfig& config, const QString& refreshToken,
                               const std::shared_ptr<net::HttpTransport>& transport);

} // namespace nesva::mail
