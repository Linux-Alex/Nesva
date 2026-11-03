// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/core/disk_space.hpp"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStorageInfo>

#include <algorithm>

namespace nesva {
namespace {

QString volumeOf(const QString& path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    return QStorageInfo(absolute).rootPath();
}

QString suggestedSubdirectory() {
#if defined(Q_OS_WIN)
    return QStringLiteral("Nesva");
#else
    return QStringLiteral("nesva");
#endif
}

} // namespace

QString humanBytes(qint64 bytes) {
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    const QStringList units = {QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB"),
                               QStringLiteral("TB")};
    double value = static_cast<double>(bytes) / 1024.0;
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(units.at(unit));
}

QVector<VolumeSpace> mountedVolumes() {
    QVector<VolumeSpace> volumes;
    for (const QStorageInfo& storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isValid() || !storage.isReady() || storage.isReadOnly()) {
            continue;
        }
        VolumeSpace volume;
        volume.rootPath = storage.rootPath();
        volume.displayName = storage.displayName();
        volume.freeBytes = storage.bytesAvailable();
        volume.totalBytes = storage.bytesTotal();
        volumes.append(volume);
    }
    std::sort(volumes.begin(), volumes.end(),
              [](const VolumeSpace& left, const VolumeSpace& right) {
                  return left.freeBytes > right.freeBytes;
              });
    return volumes;
}

QString suggestAlternativeLocation(const QString& currentPath, qint64 neededBytes) {
    const QString currentVolume = volumeOf(currentPath);
    QStringList candidates;
    for (const VolumeSpace& volume : mountedVolumes()) {
        if (volume.rootPath.compare(currentVolume, Qt::CaseInsensitive) == 0) {
            continue; // same disk: moving there does not gain space
        }
        if (volume.freeBytes >= neededBytes) {
            candidates.append(volume.rootPath);
        }
    }
    for (const QString& root : candidates) {
        QString candidate = QDir(root).absoluteFilePath(suggestedSubdirectory());
#if defined(Q_OS_WIN)
        // A drive root already ends with a separator; keep the path clean.
        candidate = QDir::cleanPath(candidate);
#endif
        if (QFileInfo(candidate).exists() || QFileInfo(root).isDir()) {
            return QDir::toNativeSeparators(candidate);
        }
    }
    return {};
}

Status requireFreeSpace(const QString& directory, qint64 neededBytes, const QString& what) {
    if (neededBytes <= 0) {
        return Status();
    }
    QStorageInfo storage(directory);
    if (!storage.isValid() || !storage.isReady()) {
        return Status(); // unknown: do not block on missing information
    }
    const qint64 available = storage.bytesAvailable();
    if (available >= neededBytes) {
        return Status();
    }
    QString detail = QStringLiteral("%1 needs about %2 but %3 has only %4 free")
                         .arg(what, humanBytes(neededBytes), directory, humanBytes(available));
    const QString alternative = suggestAlternativeLocation(directory, neededBytes);
    if (!alternative.isEmpty()) {
        detail += QStringLiteral(". %1 has enough room - consider using it as the destination.")
                      .arg(alternative);
    }
    return makeError(ErrorCode::IoError, "not enough free disk space", detail.toStdString());
}

} // namespace nesva
