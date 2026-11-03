// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "time_display.hpp"

namespace nesva::ui {
namespace {

QDateTime parseStoredUtc(const QString& isoUtc) {
    QDateTime parsed = QDateTime::fromString(isoUtc, Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(isoUtc, Qt::ISODate);
    }
    if (!parsed.isValid()) {
        return {};
    }
    return parsed.toUTC();
}

} // namespace

QTimeZone resolveDisplayTimeZone(const QString& configuredId) {
    if (configuredId.trimmed().isEmpty()) {
        return QTimeZone::systemTimeZone();
    }
    const QTimeZone zone(configuredId.trimmed().toUtf8());
    return zone.isValid() ? zone : QTimeZone::systemTimeZone();
}

QString formatInZone(const QString& isoUtc, const QTimeZone& zone, const QString& pattern) {
    const QDateTime utc = parseStoredUtc(isoUtc);
    if (!utc.isValid()) {
        return QStringLiteral("\u2014");
    }
    const QTimeZone target = zone.isValid() ? zone : QTimeZone::systemTimeZone();
    return utc.toTimeZone(target).toString(pattern);
}

QString zoneDisplayName(const QTimeZone& zone, bool systemDefault) {
    const QString id = QString::fromUtf8(zone.id());
    return systemDefault ? QStringLiteral("System (%1)").arg(id) : id;
}

} // namespace nesva::ui
