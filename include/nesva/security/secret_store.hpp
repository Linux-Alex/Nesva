// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <memory>

#include "nesva/core/result.hpp"

namespace nesva::security {

/// Stores secrets in the operating system's credential store.
///
/// Nesva never writes a password, token or passphrase into SQLite, a config
/// file or a log: the database only keeps a reference (provider/account), and
/// the value lives here.
///
/// Backends:
///  * Linux   - freedesktop.org Secret Service (GNOME Keyring, KWallet via
///              ksecretd, or any compatible provider) over D-Bus;
///  * Windows - Credential Manager (per-user, DPAPI protected);
///  * none    - reported as unsupported instead of silently degrading to a
///              plaintext file.
class SecretStore {
public:
    virtual ~SecretStore() = default;

    /// Stable identifier written to storage_credentials_refs.keyring_backend.
    virtual QString backendName() const = 0;

    /// True when a usable secret service answered the session handshake.
    virtual bool isAvailable() const = 0;

    /// Why the backend is unavailable (for diagnostics). Empty when usable.
    virtual QString unavailableReason() const { return {}; }

    virtual Status store(const QString& service, const QString& account,
                         const QByteArray& secret) = 0;

    virtual Result<QByteArray> lookup(const QString& service, const QString& account) = 0;

    virtual Status remove(const QString& service, const QString& account) = 0;
};

/// Creates the backend for the current platform.
///
/// Never throws: an unavailable backend returns a store whose isAvailable() is
/// false and whose unavailableReason() explains why, so callers can report it
/// instead of crashing or falling back to an insecure path.
std::unique_ptr<SecretStore> createSecretStore();

/// Reference text stored in SQLite for a given service/account pair.
QString secretReference(const QString& service, const QString& account);

/// Parses a stored reference back into service/account.
bool parseSecretReference(const QString& reference, QString* service, QString* account);

} // namespace nesva::security
