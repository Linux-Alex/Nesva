// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>

#include "nesva/core/result.hpp"

namespace nesva::agent {

/// Installs the background agent so it starts with the user's session.
///
/// Everything stays per-user, because the agent needs the user's profile,
/// keyring and cloud credentials:
///  * Linux: a systemd user unit in \c ~/.config/systemd/user, enabled and
///    started with \c systemctl --user (no root);
///  * Windows: a value under \c HKCU\Software\Microsoft\Windows\CurrentVersion\Run,
///    which runs at logon in the interactive session without elevation.
///
/// On Windows this deliberately avoids PowerShell and \c schtasks: both are
/// commonly blocked by group policy, while the per-user Run key is not.
Result<QString> installAutostart(const QString& agentExecutable);

/// Removes the autostart entry. Returns a description of what was removed.
Result<QString> removeAutostart();

/// Describes the current autostart state for the UI ("installed at ..." /
/// "not installed").
QString autostartDescription();

/// The path of the executable the autostart entry points at, empty when there is
/// no entry.
QString autostartExecutable();

} // namespace nesva::agent
