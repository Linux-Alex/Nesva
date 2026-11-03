// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/mail/session.hpp"

#include <QtCore/QJsonObject>

#include "nesva/core/http.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/db/database.hpp"
#include "nesva/mail/oauth.hpp"
#include "nesva/security/secret_store.hpp"

namespace nesva::mail {
namespace {

constexpr const char* kKeyringService = "nesva";

} // namespace

QString tokenAccountFor(const QString& tenantId, const QString& upn) {
    return QStringLiteral("msgraph/%1/%2").arg(tenantId, upn);
}

Result<QByteArray> loadRefreshToken(const QString& tenantId, const QString& upn) {
    std::unique_ptr<security::SecretStore> secrets = security::createSecretStore();
    if (!secrets->isAvailable()) {
        return makeErrorValue(ErrorCode::Unsupported, "no credential store is available",
                              secrets->unavailableReason().toStdString());
    }
    return secrets->lookup(QString::fromUtf8(kKeyringService), tokenAccountFor(tenantId, upn));
}

Status storeRefreshToken(const QString& tenantId, const QString& upn, const QString& token) {
    std::unique_ptr<security::SecretStore> secrets = security::createSecretStore();
    if (!secrets->isAvailable()) {
        return makeError(ErrorCode::Unsupported, "no credential store is available",
                         secrets->unavailableReason().toStdString());
    }
    return secrets->store(QString::fromUtf8(kKeyringService), tokenAccountFor(tenantId, upn),
                          token.toUtf8());
}

Result<MailConnection> connectAccount(db::Database& database, const QString& upn,
                                      const QString& tenantId, const QString& clientId,
                                      const std::function<void(const QString&)>& onUrl,
                                      int timeoutMs) {
    if (upn.trimmed().isEmpty()) {
        return makeErrorValue(ErrorCode::InvalidArgument, "an account (UPN) is required");
    }
    if (tenantId.trimmed().isEmpty() || clientId.trimmed().isEmpty()) {
        return makeErrorValue(ErrorCode::ConfigurationError,
                              "the Microsoft 365 tenant and client id must be configured",
                              "Settings -> Microsoft 365");
    }

    OAuthConfig config;
    config.tenantId = tenantId.trimmed();
    config.clientId = clientId.trimmed();

    net::QNetworkHttpTransport transport;
    const Result<TokenSet> tokens = interactiveAuthorize(
        config, std::make_shared<net::QNetworkHttpTransport>(), timeoutMs, onUrl);
    if (!tokens) {
        return tokens.error();
    }
    if (tokens.value().refreshToken.isEmpty()) {
        return makeErrorValue(ErrorCode::PermissionDenied,
                              "Microsoft did not return a refresh token; the offline_access scope "
                              "must be granted");
    }
    const Status stored = storeRefreshToken(config.tenantId, upn.trimmed(),
                                            tokens.value().refreshToken);
    if (!stored.ok()) {
        return stored.error();
    }

    // Verify the sign-in and learn the real UPN and display name.
    auto provider = std::make_shared<TokenProvider>(config, tokens.value().refreshToken,
                                                    std::make_shared<net::QNetworkHttpTransport>());
    auto client = std::make_shared<GraphClient>(provider);
    const Result<QJsonObject> me = client->me();
    const QString resolvedUpn = me ? me.value().value(QStringLiteral("userPrincipalName")).toString()
                                   : upn.trimmed();

    MailStore store(database);
    MailAccount account;
    account.displayName = me ? me.value().value(QStringLiteral("displayName")).toString() : QString();
    if (account.displayName.isEmpty()) {
        account.displayName = resolvedUpn;
    }
    account.tenantId = config.tenantId;
    account.clientId = config.clientId;
    account.accountIdentifier = resolvedUpn.isEmpty() ? upn.trimmed() : resolvedUpn;
    account.scopes = config.scopes.join(QLatin1Char(' '));
    account.status = QStringLiteral("connected");
    const Result<qint64> accountId = store.upsertAccount(account);
    if (!accountId) {
        return accountId.error();
    }

    // Keep the stored token keyed by the UPN Microsoft reported, so a later
    // refresh finds it even if the user typed a different alias.
    if (!resolvedUpn.isEmpty() && resolvedUpn != upn.trimmed()) {
        storeRefreshToken(config.tenantId, resolvedUpn, tokens.value().refreshToken);
    }

    MailConnection connection;
    connection.accountId = accountId.value();
    connection.accountIdentifier = account.accountIdentifier;
    connection.displayName = account.displayName;

    const Result<QVector<MailFolderInfo>> folders = client->mailFolders();
    if (!folders) {
        return folders.error();
    }
    const Status foldersStored = store.storeFolders(accountId.value(), folders.value());
    if (!foldersStored.ok()) {
        return foldersStored.error();
    }
    connection.folders = static_cast<int>(folders.value().size());
    return connection;
}

Result<std::shared_ptr<GraphClient>> openGraphClient(const MailAccount& account) {
    if (account.tenantId.isEmpty() || account.clientId.isEmpty()) {
        return makeErrorValue(ErrorCode::ConfigurationError,
                              "the account has no tenant or client id");
    }
    const Result<QByteArray> token = loadRefreshToken(account.tenantId, account.accountIdentifier);
    if (!token) {
        return makeErrorValue(ErrorCode::PermissionDenied,
                              "no Microsoft 365 refresh token is stored for this account",
                              QString::fromStdString(token.error().message()).toStdString());
    }

    OAuthConfig config;
    config.tenantId = account.tenantId;
    config.clientId = account.clientId;

    auto transport = std::make_shared<net::QNetworkHttpTransport>();
    auto provider = std::make_shared<TokenProvider>(config, QString::fromUtf8(token.value()),
                                                    transport);
    // Microsoft rotates refresh tokens; losing the new one would sign the user
    // out at the next run.
    const QString tenant = account.tenantId;
    const QString upn = account.accountIdentifier;
    provider->onRefreshTokenChanged = [tenant, upn](const QString& refreshed) {
        const Status stored = storeRefreshToken(tenant, upn, refreshed);
        if (!stored.ok()) {
            logWarn("mail.session", QStringLiteral("cannot store the rotated refresh token"),
                    QJsonObject{{QStringLiteral("account"), upn}});
        }
    };
    return std::make_shared<GraphClient>(provider, transport);
}

} // namespace nesva::mail
