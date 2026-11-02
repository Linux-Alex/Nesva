// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include "nesva/security/secret_store.hpp"

namespace nesva::security {

/// freedesktop.org Secret Service backend (org.freedesktop.secrets).
///
/// Works with GNOME Keyring, KWallet's ksecretd and any other implementation of
/// the Secret Service API. A locked collection that requires user interaction
/// is reported as ErrorCode::PermissionDenied rather than hanging, because the
/// agent may run without a desktop session.
class SecretServiceStore final : public SecretStore {
public:
    SecretServiceStore();
    ~SecretServiceStore() override = default;

    QString backendName() const override { return QStringLiteral("linux_secret_service"); }
    bool isAvailable() const override { return available_; }
    QString unavailableReason() const override { return reason_; }

    Status store(const QString& service, const QString& account,
                 const QByteArray& secret) override;
    Result<QByteArray> lookup(const QString& service, const QString& account) override;
    Status remove(const QString& service, const QString& account) override;

private:
    bool available_ = false;
    QString reason_;
    QString sessionPath_;
};

} // namespace nesva::security
