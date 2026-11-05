// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>

#include "nesva/core/result.hpp"

namespace nesva::ui {

/// The command-line argument that starts the window hidden in the notification
/// area. The per-user autostart entry passes it to the desktop executable.
inline constexpr const char* kStartInTrayArgument = "--tray";

/// Command line registered for the current executable, for example
/// `"C:\\Program Files\\Nesva\\nesva.exe" --tray`.
QString appAutostartCommand();

/// True when this user has an autostart entry for the desktop application.
///
/// The operating system's autostart store is the source of truth: checking the
/// startup tab and removing the entry there must be reflected in Settings.
bool appAutostartInstalled();

/// Registers the desktop application to start at sign-in, hidden in the
/// notification area. Per-user on both platforms; never needs elevation.
Status installAppAutostart();

/// Removes the desktop application's autostart entry.
Status removeAppAutostart();

/// Human-readable state for the Settings page ("not installed" /
/// "enabled: ...").
QString appAutostartDescription();

} // namespace nesva::ui
