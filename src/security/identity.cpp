// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/security/identity.hpp"

#include <QtCore/QJsonObject>
#include <QtCore/QStringList>

#include <string>
#include <utility>

#include "nesva/core/logging.hpp"

#if defined(Q_OS_WIN)
// windows.h first: sddl.h and aclapi.h depend on its types.
#include <windows.h>

#include <aclapi.h> // SetNamedSecurityInfoW, GetNamedSecurityInfoW
#include <sddl.h>   // ConvertSidToStringSidW, ConvertStringSidToSidW
#else
#include <unistd.h>
#endif

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

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

namespace {

#if defined(Q_OS_WIN)
/// Owns a SID allocated by ConvertStringSidToSidW.
struct OwnedSid {
    PSID sid = nullptr;
    ~OwnedSid() {
        if (sid != nullptr) {
            ::LocalFree(sid);
        }
    }
    OwnedSid() = default;
    OwnedSid(const OwnedSid&) = delete;
    OwnedSid& operator=(const OwnedSid&) = delete;
};
#endif

} // namespace

Status restrictFileToCurrentUser(const QString& path) {
    if (path.isEmpty()) {
        return makeError(ErrorCode::InvalidArgument, "no path given to restrict");
    }
#if defined(Q_OS_WIN)
    const ProcessIdentity identity = currentProcessIdentity();
    OwnedSid owner;
    if (identity.userId.isEmpty() ||
        ::ConvertStringSidToSidW(reinterpret_cast<LPCWSTR>(identity.userId.utf16()),
                                 &owner.sid) == 0) {
        return makeError(ErrorCode::PermissionDenied,
                         "cannot resolve the current user's security identifier",
                         identity.userId.toStdString());
    }

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(owner.sid);

    PACL acl = nullptr;
    if (::SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) {
        return makeError(ErrorCode::PermissionDenied, "cannot build a private access list",
                         path.toStdString());
    }
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const DWORD status =
        ::SetNamedSecurityInfoW(const_cast<LPWSTR>(native.c_str()), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, acl, nullptr);
    ::LocalFree(acl);
    if (status != ERROR_SUCCESS) {
        return makeError(ErrorCode::PermissionDenied, "cannot restrict the file to this user",
                         QStringLiteral("%1: error %2").arg(path).arg(status).toStdString());
    }
    return Status();
#else
    if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return makeError(ErrorCode::PermissionDenied, "cannot restrict the file to this user",
                         path.toStdString());
    }
    return Status();
#endif
}

bool fileIsOwnerOnly(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists()) {
        return false;
    }
#if defined(Q_OS_WIN)
    // Ask the DACL directly: Qt derives permissions() from the read-only attribute
    // on Windows and reports a fixed set that says nothing about other users.
    PSID ownerSid = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    if (::GetNamedSecurityInfoW(const_cast<LPWSTR>(native.c_str()), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION, &ownerSid, nullptr, &dacl, nullptr,
                                &descriptor) != ERROR_SUCCESS) {
        return false;
    }
    bool restricted = false;
    if (dacl != nullptr && dacl->AceCount == 1) {
        LPVOID ace = nullptr;
        if (::GetAce(dacl, 0, &ace) != 0) {
            const auto* header = static_cast<ACCESS_ALLOWED_ACE*>(ace);
            restricted = (header->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
        }
    }
    ::LocalFree(descriptor);
    return restricted;
#else
    const QFileDevice::Permissions permissions = info.permissions();
    const QFileDevice::Permissions others =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
        QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    return (permissions & others) == 0;
#endif
}

void setIdentityProviderForTests(IdentityProvider provider) {
    providerRef() = std::move(provider);
}

void resetIdentityProviderForTests() {
    providerRef() = IdentityProvider();
}

} // namespace nesva::security
