// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>
#include <QtCore/Qt>

#include <optional>

class QApplication;

namespace nesva::ui {

/// User-selectable theme preference. "System" follows the platform colour scheme.
enum class ThemeMode { System, Light, Dark };

QString themeModeToString(ThemeMode mode);
std::optional<ThemeMode> themeModeFromString(const QString& value);

/// Resolves the colour scheme that a theme mode actually produces right now.
Qt::ColorScheme effectiveColorScheme(ThemeMode mode);

/// Applies palette and application stylesheet for the given mode.
///
/// The Fusion style is installed by main() so that palettes are honoured
/// consistently across Linux desktop environments and Windows.
void applyTheme(QApplication& app, ThemeMode mode);

} // namespace nesva::ui
