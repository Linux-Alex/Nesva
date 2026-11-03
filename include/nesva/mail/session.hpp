// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <functional>
#include <memory>

#include "nesva/core/result.hpp"
#include "nesva/mail/graph.hpp"
#include "nesva/mail/mail_store.hpp"

namespace nesva::db {
class Database;
}

namespace nesva::mail {

/// Keyring account name for an account's refresh token. The tenant and the UPN
/// keep two accounts that belong to different tenants apart.
QString tokenAccountFor(const QString& tenantId, const QString& upn);

/// Reads the stored refresh token. Never returns the access token: that one
/// exists only in memory, for the lifetime of the client.
Result<QByteArray> loadRefreshToken(const QString& tenantId, const QString& upn);

/// Stores (or rotates) the refresh token in the OS keyring.
Status storeRefreshToken(const QString& tenantId, const QString& upn, const QString& token);

/// What a completed sign-in produced.
struct MailConnection {
    qint64 accountId = 0;
    QString accountIdentifier; ///< user principal name as Microsoft reports it
    QString displayName;
    int folders = 0;
};

/// Runs the full "connect an account" flow: interactive browser sign-in with
/// PKCE, refresh token into the keyring, then the account and its folder tree
/// into the database. Shared by the CLI and the desktop dialog so both behave
/// identically.
///
/// param onUrl Called with the sign-in URL so a caller can show it.
Result<MailConnection> connectAccount(db::Database& database, const QString& upn,
                                      const QString& tenantId, const QString& clientId,
                                      const std::function<void(const QString&)>& onUrl,
                                      int timeoutMs = 300000);

/// Builds an authenticated Graph client for a stored account.
///
/// A rotated refresh token is written back to the keyring through the provider's
/// callback, so a long-lived process does not lose the account when Microsoft
/// rotates the token. The access token stays in memory and is never persisted.
Result<std::shared_ptr<GraphClient>> openGraphClient(const MailAccount& account);

} // namespace nesva::mail
