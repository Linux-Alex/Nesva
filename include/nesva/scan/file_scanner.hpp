// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <functional>

#include "nesva/core/cancellation.hpp"
#include "nesva/core/result.hpp"
#include "nesva/scan/exclusions.hpp"

namespace nesva::scan {

enum class FileKind {
    Regular,
    Symlink, ///< Recorded but never followed by default.
    Special, ///< FIFOs, sockets, devices: detected and skipped.
};

struct ScanOptions {
    /// Absolute paths that must never be entered (the destination directory,
    /// the cache directory, sync folders). Compared after canonicalisation, and
    /// also matched when a nested directory resolves inside one of them.
    QStringList excludedRoots;

    /// User-defined exclusion rules.
    ExclusionRules exclusions;

    /// Follow symbolic links. Off by default: following links can traverse
    /// outside the selected tree or loop forever.
    bool followSymlinks = false;

    /// Include hidden files and directories (dot-files on Linux,
    /// hidden attribute on Windows).
    bool includeHidden = true;

    /// Depth limit protecting against pathological trees; a directory beyond it
    /// is recorded as skipped rather than silently ignored.
    int maxDepth = 64;

    /// Emit a progress snapshot every N files scanned (0 disables callbacks).
    int progressInterval = 64;

    /// Skip files that exist only as cloud placeholders (iCloud Drive, OneDrive,
    /// Dropbox "online-only" files). Reading one hydrates it from the cloud,
    /// which can fill the disk and start a mass download - so they are skipped
    /// and reported instead. On by default.
    bool skipCloudPlaceholders = true;

    /// Skip directories that are cloud-sync roots (iCloud Drive, OneDrive, ...):
    /// archiving one means archiving somebody else's cloud copy, and reading it
    /// can pull the whole account down. On by default.
    bool skipCloudRoots = true;

    /// Test seam: overrides the platform placeholder probe. Empty means "use the
    /// real one" (Windows file attributes; nothing is a placeholder elsewhere).
    std::function<bool(const QString& absolutePath)> placeholderProbe;

    /// Test seam: overrides the cloud-root probe.
    std::function<bool(const QString& absolutePath)> cloudRootProbe;
};

/// True when the path is a cloud placeholder rather than local content.
bool isCloudPlaceholder(const QString& absolutePath);

/// True when the path is a cloud-sync root directory that must not be walked.
bool isCloudSyncRoot(const QString& absolutePath);

struct ScanEntry {
    QString absolutePath;
    QString relativePath; ///< Relative to the source root, '/' separated.
    QString sourceRoot;
    qint64 sizeBytes = 0;
    QDateTime modifiedUtc;
    FileKind kind = FileKind::Regular;
};

struct ScanSkip {
    QString path;
    QString reason;
};

/// A source root plus the label used to build collision-free remote paths.
struct ScanSource {
    QString root;
    QString label;
};

struct ScanResult {
    QVector<ScanEntry> files;
    QVector<ScanSkip> skipped;
    qint64 totalBytes = 0;
    int directoriesVisited = 0;
    bool cancelled = false;
    bool truncated = false;

    int fileCount() const { return static_cast<int>(files.size()); }
};

using ScanProgress = std::function<void(const ScanResult&)>;

/// Walks sources and returns the files that should be archived.
///
/// Guarantees:
///  * never descends into excluded roots, symlinked directories (unless
///    enabled) or directories beyond the depth limit;
///  * detects symlink/reparse-point loops when following links;
///  * records every skipped entry with a human-readable reason, so a job report
///    can explain what was *not* archived;
///  * returns ErrorCode::InvalidArgument for a missing/not-a-directory source and
///    ErrorCode::Cancelled when the token is cancelled;
///  * never modifies anything on disk.
Result<ScanResult> scanSources(const QVector<ScanSource>& sources, const ScanOptions& options,
                               const CancellationToken& cancellation,
                               const ScanProgress& progress = {});

/// True when \p candidate is equal to, or inside, any of \p roots.
bool isInsideAnyRoot(const QString& candidate, const QStringList& roots);

} // namespace nesva::scan
