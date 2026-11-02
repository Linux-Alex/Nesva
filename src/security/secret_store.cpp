// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/security/secret_store.hpp"

#include <QtCore/QStringList>

#if defined(Q_OS_WIN)
#include "nesva/security/credential_manager_windows.hpp"
#else
#include "nesva/security/secret_service_linux.hpp"
#endif

namespace nesva::security {
namespace {

class UnavailableSecretStore final : public SecretStore {
public:
    explicit UnavailableSecretStore(QString reason) : reason_(std::move(reason)) {}

    QString backendName() const override { return QStringLiteral("none"); }
    bool isAvailable() const override { return false; }
    QString unavailableReason() const override { return reason_; }

    Status store(const QString&, const QString&, const QByteArray&) override {
        return makeError(ErrorCode::Unsupported, "no credential store is available", reason_.toStdString());
    }

    Result<QByteArray> lookup(const QString&, const QString&) override {
        return makeErrorValue(ErrorCode::Unsupported, "no credential store is available",
                              reason_.toStdString());
    }

    Status remove(const QString&, const QString&) override {
        return makeError(ErrorCode::Unsupported, "no credential store is available", reason_.toStdString());
    }

private:
    QString reason_;
};

} // namespace

std::unique_ptr<SecretStore> createSecretStore() {
#if defined(Q_OS_WIN)
    auto store = std::make_unique<CredentialManagerStore>();
    if (store->isAvailable()) {
        return store;
    }
    return std::make_unique<UnavailableSecretStore>(store->unavailableReason());
#elif defined(Q_OS_LINUX)
    auto store = std::make_unique<SecretServiceStore>();
    if (store->isAvailable()) {
        return store;
    }
    const QString reason = store->unavailableReason();
    return std::make_unique<UnavailableSecretStore>(
        reason.isEmpty() ? QStringLiteral("the Secret Service is not reachable on the session bus")
                         : reason);
#else
    return std::make_unique<UnavailableSecretStore>(
        QStringLiteral("this platform has no supported credential store"));
#endif
}

QString secretReference(const QString& service, const QString& account) {
    return service + QLatin1Char('/') + account;
}

bool parseSecretReference(const QString& reference, QString* service, QString* account) {
    const int separator = reference.indexOf(QLatin1Char('/'));
    if (separator <= 0 || separator + 1 >= reference.size()) {
        return false;
    }
    if (service != nullptr) {
        *service = reference.left(separator);
    }
    if (account != nullptr) {
        *account = reference.mid(separator + 1);
    }
    return true;
}

} // namespace nesva::security
