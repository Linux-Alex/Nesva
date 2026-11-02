// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/security/credential_manager_windows.hpp"

#if defined(Q_OS_WIN)

#include <windows.h>
#include <wincred.h>

#include <QtCore/QString>

#include "nesva/core/logging.hpp"

namespace nesva::security {
namespace {

/// Credential Manager target names are capped at 32767 characters but the
/// practical limit is much smaller; a stable, namespaced name keeps entries
/// recognisable in the Windows UI.
QString targetFor(const QString& service, const QString& account) {
    return QStringLiteral("Nesva/%1/%2").arg(service, account);
}

QString errorText(DWORD code) {
    return QStringLiteral("Credential Manager error %1").arg(code);
}

} // namespace

bool CredentialManagerStore::isAvailable() const {
    return true;
}

Status CredentialManagerStore::store(const QString& service, const QString& account,
                                     const QByteArray& secret) {
    const QString target = targetFor(service, account);
    const std::wstring wideTarget = target.toStdWString();

    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(wideTarget.c_str());
    credential.Comment = const_cast<LPWSTR>(L"Nesva secret");
    credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
    credential.CredentialBlob =
        reinterpret_cast<LPBYTE>(const_cast<char*>(secret.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

    if (!::CredWriteW(&credential, 0)) {
        return makeError(ErrorCode::PermissionDenied, "cannot store the credential",
                         errorText(::GetLastError()).toStdString());
    }
    return Status();
}

Result<QByteArray> CredentialManagerStore::lookup(const QString& service, const QString& account) {
    const QString target = targetFor(service, account);
    const std::wstring wideTarget = target.toStdWString();

    PCREDENTIALW credential = nullptr;
    if (!::CredReadW(wideTarget.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_NOT_FOUND) {
            return makeErrorValue(ErrorCode::NotFound, "no stored credential for this reference",
                                  target.toStdString());
        }
        return makeErrorValue(ErrorCode::PermissionDenied, "cannot read the credential",
                              errorText(error).toStdString());
    }

    const QByteArray secret(reinterpret_cast<const char*>(credential->CredentialBlob),
                            static_cast<int>(credential->CredentialBlobSize));
    ::CredFree(credential);
    return secret;
}

Status CredentialManagerStore::remove(const QString& service, const QString& account) {
    const QString target = targetFor(service, account);
    const std::wstring wideTarget = target.toStdWString();
    if (!::CredDeleteW(wideTarget.c_str(), CRED_TYPE_GENERIC, 0)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_NOT_FOUND) {
            return Status();
        }
        return makeError(ErrorCode::PermissionDenied, "cannot delete the credential",
                         errorText(error).toStdString());
    }
    return Status();
}

} // namespace nesva::security

#endif // Q_OS_WIN
