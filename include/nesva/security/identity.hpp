// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>

#include <functional>

#include "nesva/core/result.hpp"

namespace nesva::security {

/// Who the process is actually running as.
///
/// Nesva is a per-user application: the profile it archives, the keyring it
/// reads and the schedule it writes all belong to one user. Windows services,
/// SYSTEM, LocalService and NetworkService have their own profile and no access
/// to the interactive user's credentials, so a backup started from a service
/// context silently backs up the wrong data. This is what makes that visible.
struct ProcessIdentity {
    QString userName;  ///< "ARCONT\FUJS", "root", ...
    QString userId;    ///< SID on Windows, uid on POSIX
    bool serviceAccount = false; ///< SYSTEM / LocalService / NetworkService, or root on POSIX
    bool elevated = false;       ///< administrator token / euid 0
    QString detail;              ///< why it counts as a service account
};

/// Reads the real process token.
ProcessIdentity currentProcessIdentity();

/// Fails when jobs must not run in this context (a service account). Used before
/// a run starts, so a SYSTEM-launched agent cannot archive the wrong profile.
Status requireUserContext(const QString& component);

/// Restricts a file to the current user only.
///
/// POSIX mode bits (0600) express this directly. Windows has no such bits: a file
/// in %TEMP% or %LOCALAPPDATA% inherits a DACL that normally lets other users on
/// the machine read it, so the DACL is replaced with a single entry for the
/// current user and inheritance is switched off. Needed wherever Nesva writes a
/// secret to disk, such as the temporary restic password file.
Status restrictFileToCurrentUser(const QString& path);

/// True when p path is readable only by the current user. On Windows this asks
/// whether the DACL holds exactly one entry, for this user; on POSIX whether the
/// group and other bits are clear.
bool fileIsOwnerOnly(const QString& path);

/// Test seam: replaces the platform probe.
using IdentityProvider = std::function<ProcessIdentity()>;
void setIdentityProviderForTests(IdentityProvider provider);
void resetIdentityProviderForTests();

} // namespace nesva::security
