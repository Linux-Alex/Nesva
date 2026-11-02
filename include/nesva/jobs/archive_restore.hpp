// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <functional>
#include <optional>

#include "nesva/core/cancellation.hpp"
#include "nesva/core/result.hpp"
#include "nesva/jobs/job_store.hpp"

namespace nesva::db {
class Database;
}

namespace nesva::jobs {

/// What to do when a file already exists at the restore destination.
///
/// Ask means "the caller must resolve the conflicts first"; the core refuses to
/// guess, because silently overwriting user files is never acceptable.
enum class ConflictPolicy {
    Ask,
    Skip,
    Overwrite,
    Rename,
};

QString conflictPolicyToString(ConflictPolicy policy);
ConflictPolicy conflictPolicyFromString(const QString& value, ConflictPolicy fallback = ConflictPolicy::Ask);

struct ArchiveItem {
    QString relativePath; ///< path inside the archive, '/' separated
    qint64 sizeBytes = 0;
    QDateTime modifiedUtc;
    QString sha256;       ///< empty when the provider could not verify
    QString state;        ///< manifest state when known
};

struct RestorePlan {
    QVector<ArchiveItem> items;
    int conflictingFiles = 0;
    QStringList conflictSamples;
    qint64 totalBytes = 0;

    bool hasConflicts() const { return conflictingFiles > 0; }
};

struct RestoreRequest {
    ArchiveJob job;
    std::optional<StorageProviderRecord> providerRecord;
    QString destinationDirectory;
    ConflictPolicy policy = ConflictPolicy::Ask;
    /// Empty restores everything in the plan.
    QStringList onlyPaths;
    bool verify = true;
    int maxAttempts = 3;
    int retryBackoffMs = 400;
};

struct RestoreProgress {
    QString currentFile;
    qint64 filesTotal = 0;
    qint64 filesDone = 0;
    qint64 bytesDone = 0;
    QString message;
};

struct RestoreReport {
    qint64 restoreJobId = 0;
    QString status = QStringLiteral("failed");
    int restored = 0;
    int skipped = 0;
    int renamed = 0;
    int failed = 0;
    qint64 bytesRestored = 0;
    QStringList errors;
    QStringList verifiedFiles;

    bool succeeded() const { return status == QStringLiteral("succeeded"); }
};

using RestoreProgressCallback = std::function<void(const RestoreProgress&)>;

/// Restores archived files.
///
/// Guarantees:
///  * never overwrites an existing file unless the policy is Overwrite;
///  * Ask is rejected before any file is written;
///  * every restored file is verified against the manifest hash when one is
///    known, and a mismatch removes the file and counts as a failure;
///  * the restore is recorded in restore_jobs with per-file counters;
///  * cancellation stops between files and leaves completed files intact.
class ArchiveRestorer {
public:
    ArchiveRestorer(db::Database& database, RestoreProgressCallback progress = {});

    /// Lists what can be restored. Uses the provider as the source of truth for
    /// what exists, and the manifest for hashes and states.
    /// param destinationDirectory used to detect conflicts; ignored when
    ///        p checkDestination is false (for example when browsing).
    Result<RestorePlan> plan(const ArchiveJob& job,
                             const std::optional<StorageProviderRecord>& provider,
                             const QString& destinationDirectory,
                             const QStringList& onlyPaths = {},
                             const QString& rcloneExecutable = {},
                             bool checkDestination = true);

    RestoreReport restore(const RestoreRequest& request, const CancellationToken& cancellation,
                          const QString& rcloneExecutable = {});

private:
    db::Database& database_;
    JobStore store_;
    RestoreProgressCallback progress_;
};

} // namespace nesva::jobs
