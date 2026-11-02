// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include "nesva/security/secret_store.hpp"

namespace nesva::security {

/// Windows Credential Manager backend (per-user, DPAPI protected).
///
/// Credentials are written with CRED_PERSIST_LOCAL_MACHINE scoped to the
/// current user, so they are never readable by another user account. Runs on
/// the user's own logon session: a system service cannot read them, which is
/// why the agent is registered as a per-user task.
class CredentialManagerStore final : public SecretStore {
public:
    CredentialManagerStore() = default;
    ~CredentialManagerStore() override = default;

    QString backendName() const override { return QStringLiteral("windows_credential_manager"); }
    bool isAvailable() const override;
    QString unavailableReason() const override { return reason_; }

    Status store(const QString& service, const QString& account,
                 const QByteArray& secret) override;
    Result<QByteArray> lookup(const QString& service, const QString& account) override;
    Status remove(const QString& service, const QString& account) override;

private:
    QString reason_;
};

} // namespace nesva::security
