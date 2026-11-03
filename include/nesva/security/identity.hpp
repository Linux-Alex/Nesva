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

/// Test seam: replaces the platform probe.
using IdentityProvider = std::function<ProcessIdentity()>;
void setIdentityProviderForTests(IdentityProvider provider);
void resetIdentityProviderForTests();

} // namespace nesva::security
