// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QTimeZone>

namespace nesva::ui {

/// The timezone used to display timestamps: the one chosen in Settings, or the
/// system zone when none was chosen.
QTimeZone resolveDisplayTimeZone(const QString& configuredId);

/// Formats a stored UTC timestamp ("2026-11-03T19:15:00.000Z") in \p zone.
/// Returns an em dash for empty or unparsable input, so a card never shows
/// "null" or an epoch date.
QString formatInZone(const QString& isoUtc, const QTimeZone& zone,
                     const QString& pattern = QStringLiteral("yyyy-MM-dd HH:mm"));

/// Name of the zone for display ("Europe/Ljubljana" / "System (Europe/Ljubljana)").
QString zoneDisplayName(const QTimeZone& zone, bool systemDefault);

} // namespace nesva::ui
