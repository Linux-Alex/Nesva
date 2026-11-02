// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/core/paths.hpp"

#include <QtCore/QDir>
#include <QtCore/QStandardPaths>

namespace nesva {
namespace {

QString& overrideRef() {
    static QString override;
    return override;
}

QString fallbackBase(const QStandardPaths::StandardLocation location) {
    QString base = QStandardPaths::writableLocation(location);
    if (base.isEmpty()) {
        base = QDir::homePath();
    }
    return base;
}

} // namespace

QString Paths::dataRootOverride() {
    return overrideRef();
}

void Paths::setDataRootOverride(const QString& path) {
    overrideRef() = path;
}

QString Paths::dataRoot() {
    if (!overrideRef().isEmpty()) {
        return QDir::cleanPath(overrideRef());
    }

    const QByteArray fromEnvironment = qgetenv("NESVA_DATA_DIR");
    if (!fromEnvironment.isEmpty()) {
        return QDir::cleanPath(QString::fromUtf8(fromEnvironment));
    }

    return QDir(fallbackBase(QStandardPaths::GenericDataLocation))
        .absoluteFilePath(QStringLiteral("nesva"));
}

QString Paths::logsDirectory() {
    return QDir(dataRoot()).absoluteFilePath(QStringLiteral("logs"));
}

QString Paths::cacheDirectory() {
    if (!overrideRef().isEmpty() || !qgetenv("NESVA_DATA_DIR").isEmpty()) {
        return QDir(dataRoot()).absoluteFilePath(QStringLiteral("cache"));
    }
    return QDir(fallbackBase(QStandardPaths::GenericCacheLocation))
        .absoluteFilePath(QStringLiteral("nesva"));
}

QString Paths::databasePath() {
    return QDir(dataRoot()).absoluteFilePath(QStringLiteral("nesva.db"));
}

QString Paths::logFilePath(const QString& fileName) {
    return QDir(logsDirectory()).absoluteFilePath(fileName);
}

Status Paths::ensureBaseDirectories() {
    const QStringList directories = {dataRoot(), logsDirectory(), cacheDirectory()};
    for (const QString& path : directories) {
        QDir directory(path);
        if (directory.exists()) {
            continue;
        }
        if (!directory.mkpath(QStringLiteral("."))) {
            return makeError(ErrorCode::IoError, "cannot create directory", path.toStdString());
        }
    }
    return Status();
}

QString Paths::describeResolution() {
    if (!overrideRef().isEmpty()) {
        return QStringLiteral("override: %1").arg(QDir::cleanPath(overrideRef()));
    }
    if (!qgetenv("NESVA_DATA_DIR").isEmpty()) {
        return QStringLiteral("NESVA_DATA_DIR environment variable: %1").arg(dataRoot());
    }
    return QStringLiteral("platform default: %1").arg(dataRoot());
}

} // namespace nesva
