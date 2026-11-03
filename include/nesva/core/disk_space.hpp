// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>
#include <QtCore/QVector>

#include "nesva/core/result.hpp"

namespace nesva {

/// Free space of a mounted volume.
struct VolumeSpace {
    QString rootPath;
    QString displayName;
    qint64 freeBytes = 0;
    qint64 totalBytes = 0;
};

/// Mounted volumes with usable free space, largest first.
QVector<VolumeSpace> mountedVolumes();

/// A directory on a *different* volume that can take \p neededBytes, formatted
/// for a human ("D:\Nesva" or "/mnt/backup/Nesva"). Empty when no other volume
/// has room. This is what turns "disk full" into an actionable suggestion
/// instead of an error.
QString suggestAlternativeLocation(const QString& currentPath, qint64 neededBytes);

/// Fails with an actionable message when \p directory has less than
/// \p neededBytes free.
Status requireFreeSpace(const QString& directory, qint64 neededBytes,
                        const QString& what = QStringLiteral("this operation"));

/// Human-readable byte count ("12.4 GB").
QString humanBytes(qint64 bytes);

} // namespace nesva
