// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/security/identity.hpp"

#include <QtCore/QJsonObject>

#include <utility>

#include "nesva/core/logging.hpp"

#if defined(Q_OS_WIN)
// windows.h first: sddl.h depends on its types.
#include <windows.h>

#include <sddl.h> // ConvertSidToStringSidW lives here
#else
#include <unistd.h>
#endif

namespace nesva::security {
namespace {

IdentityProvider& providerRef() {
    static IdentityProvider provider;
    return provider;
}

#if defined(Q_OS_WIN)
QString sidToString(PSID sid) {
    LPWSTR text = nullptr;
    if (::ConvertSidToStringSidW(sid, &text) == 0) {
        return QString();
    }
    const QString value = QString::fromWCharArray(text);
    ::LocalFree(text);
    return value;
}
#endif

ProcessIdentity probePlatformIdentity() {
    ProcessIdentity identity;
#if defined(Q_OS_WIN)
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        identity.userName = QStringLiteral("unknown");
        return identity;
    }
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray buffer(static_cast<int>(size), '\0');
    if (::GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0) {
        auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
        identity.userId = sidToString(user->User.Sid);
        wchar_t name[256] = {};
        wchar_t domain[256] = {};
        DWORD nameLength = 256;
        DWORD domainLength = 256;
        SID_NAME_USE use = SidTypeUnknown;
        if (::LookupAccountSidW(nullptr, user->User.Sid, name, &nameLength, domain, &domainLength,
                                &use) != 0) {
            const QString account = QString::fromWCharArray(name);
            const QString owner = QString::fromWCharArray(domain);
            identity.userName = owner.isEmpty() ? account
                                                : owner + QLatin1Char('\\') + account;
        }
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    if (::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) != 0) {
        identity.elevated = elevation.TokenIsElevated != 0;
    }
    ::CloseHandle(token);

    // S-1-5-18 SYSTEM, S-1-5-19 LOCAL SERVICE, S-1-5-20 NETWORK SERVICE.
    static const QStringList kServiceSids = {QStringLiteral("S-1-5-18"), QStringLiteral("S-1-5-19"),
                                             QStringLiteral("S-1-5-20")};
    if (kServiceSids.contains(identity.userId)) {
        identity.serviceAccount = true;
        identity.detail = QStringLiteral("running as a Windows service account (%1)")
                              .arg(identity.userId);
    }
    if (identity.userName.isEmpty()) {
        identity.userName = identity.serviceAccount ? QStringLiteral("service account")
                                                    : QStringLiteral("unknown");
    }
#else
    const uid_t uid = ::geteuid();
    identity.userId = QString::number(static_cast<qulonglong>(uid));
    identity.elevated = uid == 0;
    if (const char* user = ::getenv("USER")) {
        identity.userName = QString::fromLocal8Bit(user);
    }
    if (identity.userName.isEmpty()) {
        identity.userName = QStringLiteral("uid %1").arg(uid);
    }
    if (uid == 0) {
        identity.serviceAccount = true;
        identity.detail = QStringLiteral("running as root, not as a user session");
    }
#endif
    return identity;
}

} // namespace

ProcessIdentity currentProcessIdentity() {
    if (providerRef()) {
        return providerRef()();
    }
    return probePlatformIdentity();
}

Status requireUserContext(const QString& component) {
    const ProcessIdentity identity = currentProcessIdentity();
    if (!identity.serviceAccount) {
        return Status();
    }
    logWarn("security.identity", QStringLiteral("refusing to run under a service account"),
            QJsonObject{{QStringLiteral("component"), component},
                        {QStringLiteral("user"), identity.userName},
                        {QStringLiteral("user_id"), identity.userId}});
    return makeError(
        ErrorCode::PermissionDenied,
        "Nesva must run as the user whose data it backs up",
        QStringLiteral("%1: %2. Log in as that user and start Nesva from their session; the "
                       "profile, the keyring and the schedule are all per-user.")
            .arg(identity.detail, component)
            .toStdString());
}

void setIdentityProviderForTests(IdentityProvider provider) {
    providerRef() = std::move(provider);
}

void resetIdentityProviderForTests() {
    providerRef() = IdentityProvider();
}

} // namespace nesva::security
