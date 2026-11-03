// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <memory>

#include "nesva/core/result.hpp"
#include "nesva/mail/graph.hpp"
#include "nesva/mail/mail_store.hpp"

namespace nesva::mail {

/// Keyring account name for an account's refresh token. The tenant and the UPN
/// keep two accounts that belong to different tenants apart.
QString tokenAccountFor(const QString& tenantId, const QString& upn);

/// Reads the stored refresh token. Never returns the access token: that one
/// exists only in memory, for the lifetime of the client.
Result<QByteArray> loadRefreshToken(const QString& tenantId, const QString& upn);

/// Stores (or rotates) the refresh token in the OS keyring.
Status storeRefreshToken(const QString& tenantId, const QString& upn, const QString& token);

/// Builds an authenticated Graph client for a stored account.
///
/// A rotated refresh token is written back to the keyring through the provider's
/// callback, so a long-lived process does not lose the account when Microsoft
/// rotates the token. The access token stays in memory and is never persisted.
Result<std::shared_ptr<GraphClient>> openGraphClient(const MailAccount& account);

} // namespace nesva::mail
