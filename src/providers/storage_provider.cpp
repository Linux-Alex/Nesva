// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/providers/storage_provider.hpp"

#include <QtCore/QDir>

namespace nesva::providers {

Result<QString> normaliseRelativePath(const QString& path) {
    QString normalised = path;
    normalised.replace(QLatin1Char('\\'), QLatin1Char('/'));
    normalised = normalised.trimmed();

    if (normalised.isEmpty()) {
        return makeErrorValue(ErrorCode::InvalidArgument, "relative path must not be empty");
    }
    if (normalised.startsWith(QLatin1Char('/'))) {
        return makeErrorValue(ErrorCode::InvalidArgument, "relative path must not be absolute",
                              path.toStdString());
    }
    // Windows drive letters or UNC prefixes have no meaning in a relative path.
    if (normalised.size() > 1 && normalised.at(1) == QLatin1Char(':')) {
        return makeErrorValue(ErrorCode::InvalidArgument, "relative path must not contain a drive",
                              path.toStdString());
    }

    QStringList segments;
    for (const QString& segment : normalised.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (segment == QStringLiteral(".") || segment == QStringLiteral("..")) {
            return makeErrorValue(ErrorCode::InvalidArgument,
                                  "relative path must not contain '.' or '..' segments",
                                  path.toStdString());
        }
        segments.append(segment);
    }
    if (segments.isEmpty()) {
        return makeErrorValue(ErrorCode::InvalidArgument, "relative path resolves to nothing",
                              path.toStdString());
    }
    return segments.join(QLatin1Char('/'));
}

QString joinRelative(const QString& base, const QString& child) {
    if (base.isEmpty()) {
        return child;
    }
    if (child.isEmpty()) {
        return base;
    }
    return base.endsWith(QLatin1Char('/')) ? base + child : base + QLatin1Char('/') + child;
}

QString sanitiseForRemote(const QString& path) {
    QString value = QDir::fromNativeSeparators(path);
    while (value.startsWith(QLatin1Char('/'))) {
        value.remove(0, 1);
    }
    if (value.size() > 1 && value.at(1) == QLatin1Char(':')) {
        value.remove(0, 2);
        while (value.startsWith(QLatin1Char('/'))) {
            value.remove(0, 1);
        }
    }
    return value;
}

} // namespace nesva::providers
