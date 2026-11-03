// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/scan/file_scanner.hpp"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace nesva::scan {
namespace {

QString normalisedPath(const QString& path) {
    const QString cleaned = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return cleaned.toLower();
#else
    return cleaned;
#endif
}

/// Canonical path when it can be resolved, otherwise the absolute path.
/// Canonicalisation is what makes symlink-loop detection work: a link that
/// points back into an ancestor resolves to a path that was already visited.
QString canonicalOrAbsolute(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return normalisedPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

bool isLink(const QFileInfo& info) {
    return info.isSymLink() || info.isJunction();
}

/// Windows marks cloud placeholders with FILE_ATTRIBUTE_OFFLINE or one of the
/// recall-on-access attributes; reading such a file downloads it. Other
/// platforms have no equivalent, so the portable answer is "no".
bool isCloudPlaceholderDefault(const QString& absolutePath) {
#if defined(Q_OS_WIN)
    const std::wstring wide = QDir::toNativeSeparators(absolutePath).toStdWString();
    const DWORD attributes = ::GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
#ifdef FILE_ATTRIBUTE_RECALL_ON_OPEN
    constexpr DWORD kRecallOnOpen = FILE_ATTRIBUTE_RECALL_ON_OPEN;
#else
    constexpr DWORD kRecallOnOpen = 0x00040000;
#endif
#ifdef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
    constexpr DWORD kRecallOnDataAccess = FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
#else
    constexpr DWORD kRecallOnDataAccess = 0x00400000;
#endif
    return (attributes &
            (static_cast<DWORD>(FILE_ATTRIBUTE_OFFLINE) | kRecallOnOpen | kRecallOnDataAccess)) != 0;
#else
    Q_UNUSED(absolutePath);
    return false;
#endif
}

/// Cloud sync roots: reparse points on Windows, plus the known folder names that
/// appear on every platform.
bool isCloudRootDefault(const QString& absolutePath) {
#if defined(Q_OS_WIN)
    const std::wstring wide = QDir::toNativeSeparators(absolutePath).toStdWString();
    const DWORD attributes = ::GetFileAttributesW(wide.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return true;
    }
#else
    Q_UNUSED(absolutePath);
#endif
    const QString name = QFileInfo(absolutePath).fileName();
    static const QStringList kCloudNames = {
        QStringLiteral("iCloudDrive"), QStringLiteral("OneDrive"),   QStringLiteral("Dropbox"),
        QStringLiteral("Google Drive"), QStringLiteral("pCloudDrive"), QStringLiteral("Nextcloud"),
        QStringLiteral("MEGAsync"),
    };
    for (const QString& known : kCloudNames) {
        if (name.compare(known, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return name.startsWith(QStringLiteral("OneDrive - "), Qt::CaseInsensitive);
}

} // namespace

bool isCloudPlaceholder(const QString& absolutePath) {
    return isCloudPlaceholderDefault(absolutePath);
}

bool isCloudSyncRoot(const QString& absolutePath) {
    return isCloudRootDefault(absolutePath);
}

bool isInsideAnyRoot(const QString& candidate, const QStringList& roots) {
    if (candidate.isEmpty() || roots.isEmpty()) {
        return false;
    }
    const QString normalisedCandidate = canonicalOrAbsolute(candidate);
    for (const QString& root : roots) {
        const QString normalisedRoot = canonicalOrAbsolute(root);
        if (normalisedRoot.isEmpty()) {
            continue;
        }
        if (normalisedCandidate == normalisedRoot) {
            return true;
        }
        const QString prefix =
            normalisedRoot.endsWith(QLatin1Char('/')) ? normalisedRoot : normalisedRoot + QLatin1Char('/');
        if (normalisedCandidate.startsWith(prefix)) {
            return true;
        }
    }
    return false;
}

Result<ScanResult> scanSources(const QVector<ScanSource>& sources, const ScanOptions& options,
                               const CancellationToken& cancellation,
                               const ScanProgress& progress) {
    if (sources.isEmpty()) {
        return makeErrorValue(ErrorCode::InvalidArgument, "no sources configured");
    }

    ScanResult result;
    int sinceProgress = 0;

    QStringList excludedRoots;
    excludedRoots.reserve(options.excludedRoots.size());
    for (const QString& root : options.excludedRoots) {
        if (!root.trimmed().isEmpty()) {
            excludedRoots.append(root);
        }
    }

    struct DirectoryTask {
        QString path;
        QString relative;
        int depth = 0;
    };

    for (const ScanSource& source : sources) {
        if (cancellation.isCancelled()) {
            result.cancelled = true;
            return result;
        }

        const QFileInfo rootInfo(source.root);
        if (!rootInfo.exists()) {
            return makeErrorValue(ErrorCode::NotFound, "source directory does not exist",
                                  source.root.toStdString());
        }
        if (!rootInfo.isDir()) {
            return makeErrorValue(ErrorCode::InvalidArgument, "source is not a directory",
                                  source.root.toStdString());
        }

        const QString rootPath = QDir::cleanPath(rootInfo.absoluteFilePath());
        const QString canonicalRoot = canonicalOrAbsolute(rootPath);
        if (isInsideAnyRoot(canonicalRoot, excludedRoots)) {
            result.skipped.append({rootPath, QStringLiteral("source is inside an excluded root")});
            continue;
        }

        QSet<QString> visitedDirectories;
        QVector<DirectoryTask> pending;
        pending.append({rootPath, QString(), 0});

        while (!pending.isEmpty()) {
            if (cancellation.isCancelled()) {
                result.cancelled = true;
                return result;
            }

            const DirectoryTask task = pending.takeLast();
            ++result.directoriesVisited;

            const QFileInfo directoryInfo(task.path);
            if (!directoryInfo.isDir()) {
                result.skipped.append({task.path, QStringLiteral("not a directory")});
                continue;
            }

            if (options.followSymlinks) {
                const QString identity = canonicalOrAbsolute(task.path);
                if (visitedDirectories.contains(identity)) {
                    result.skipped.append({task.path, QStringLiteral("symlink loop detected")});
                    continue;
                }
                visitedDirectories.insert(identity);
            }

            QDir directory(task.path);
            directory.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            const QFileInfoList entries = directory.entryInfoList();

            if (entries.isEmpty() && !directoryInfo.isReadable()) {
                result.skipped.append({task.path, QStringLiteral("directory is not readable")});
                continue;
            }

            for (const QFileInfo& entry : entries) {
                if (cancellation.isCancelled()) {
                    result.cancelled = true;
                    return result;
                }

                const QString relative = task.relative.isEmpty()
                                             ? entry.fileName()
                                             : task.relative + QLatin1Char('/') + entry.fileName();

                if (options.exclusions.matches(relative, entry.fileName())) {
                    result.skipped.append(
                        {entry.absoluteFilePath(), QStringLiteral("excluded by pattern")});
                    continue;
                }

                const bool link = isLink(entry);

                if (entry.isDir()) {
                    if (link && !options.followSymlinks) {
                        result.skipped.append({entry.absoluteFilePath(),
                                               QStringLiteral("symlink directory not followed")});
                        continue;
                    }
                    if (options.skipCloudRoots) {
                        const auto probe = options.cloudRootProbe ? options.cloudRootProbe
                                                                  : isCloudSyncRoot;
                        if (probe(entry.absoluteFilePath())) {
                            result.skipped.append(
                                {entry.absoluteFilePath(),
                                 QStringLiteral("cloud sync folder not followed")});
                            continue;
                        }
                    }
                    if (isInsideAnyRoot(entry.absoluteFilePath(), excludedRoots)) {
                        result.skipped.append({entry.absoluteFilePath(),
                                               QStringLiteral("destination or excluded root")});
                        continue;
                    }
                    if (task.depth + 1 > options.maxDepth) {
                        result.skipped.append(
                            {entry.absoluteFilePath(), QStringLiteral("maximum depth exceeded")});
                        continue;
                    }
                    pending.append({entry.absoluteFilePath(), relative, task.depth + 1});
                    continue;
                }

                if (link) {
                    result.skipped.append(
                        {entry.absoluteFilePath(), QStringLiteral("symlink not followed")});
                    continue;
                }

                if (!entry.isFile()) {
                    result.skipped.append({entry.absoluteFilePath(),
                                           QStringLiteral("special file (not a regular file)")});
                    continue;
                }

                if (!options.includeHidden && entry.isHidden()) {
                    result.skipped.append({entry.absoluteFilePath(), QStringLiteral("hidden file")});
                    continue;
                }

                if (options.skipCloudPlaceholders) {
                    // Reading a placeholder hydrates it: the file would be
                    // downloaded from iCloud/OneDrive during the backup.
                    const auto probe = options.placeholderProbe ? options.placeholderProbe
                                                                : isCloudPlaceholder;
                    if (probe(entry.absoluteFilePath())) {
                        result.skipped.append({entry.absoluteFilePath(),
                                               QStringLiteral("cloud placeholder not downloaded")});
                        continue;
                    }
                }

                ScanEntry scanned;
                scanned.absolutePath = entry.absoluteFilePath();
                scanned.relativePath = relative;
                scanned.sourceRoot = rootPath;
                scanned.sizeBytes = entry.size();
                scanned.modifiedUtc = entry.lastModified().toUTC();
                scanned.kind = FileKind::Regular;
                result.totalBytes += scanned.sizeBytes;
                result.files.append(scanned);

                ++sinceProgress;
                if (options.progressInterval > 0 && progress && sinceProgress >= options.progressInterval) {
                    sinceProgress = 0;
                    progress(result);
                }
            }
        }
    }

    if (progress) {
        progress(result);
    }
    return result;
}

} // namespace nesva::scan
