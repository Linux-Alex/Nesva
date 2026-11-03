// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/mail/session.hpp"

#include <QtCore/QJsonObject>

#include "nesva/core/http.hpp"
#include "nesva/core/logging.hpp"
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
