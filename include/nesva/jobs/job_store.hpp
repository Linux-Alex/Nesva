// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <optional>

#include "nesva/backup/restic.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/core/result.hpp"
#include "nesva/scan/exclusions.hpp"
#include "nesva/scheduler/schedule.hpp"

namespace nesva::db {
class Database;
}

namespace nesva::jobs {

/// Configured destination as stored in "storage_providers".
struct StorageProviderRecord {
    qint64 id = 0;
    QString name;
    QString providerType;   ///< 'local' | 'rclone' | 'icloud' | 's3' | 'sftp' | 'webdav' | 'other'
    QString rcloneRemote;   ///< e.g. "icloud:" for rclone-backed remotes
    QString basePath;       ///< absolute for local, sub-directory for remotes
    QString status;
    QString statusDetail;
    QString lastCheckedAt;

    bool isRcloneBacked() const {
        return providerType == QStringLiteral("rclone") || providerType == QStringLiteral("icloud") ||
               providerType == QStringLiteral("s3") || providerType == QStringLiteral("sftp") ||
               providerType == QStringLiteral("webdav");
    }
};

struct JobSource {
    qint64 id = 0;
    QString path;
    QString label; ///< namespace on the destination; derived from the path name when empty
};

/// A complete archive job definition.
struct ArchiveJob {
    qint64 id = 0;
    QString name;
    QString mode = QStringLiteral("archive");
    std::optional<qint64> storageProviderId;
    QString destinationPath; ///< sub-directory inside the provider
    bool enabled = true;
    int bandwidthLimitKiB = 0;
    bool verifyChecksum = true;
    QVector<JobSource> sources;
    QVector<scan::ExclusionRule> exclusions;

    // Backup mode (Restic) options. destinationPath holds the repository
    // location ("/srv/restic/home" or "rclone:icloud:Nesva/restic").
    backup::RetentionPolicy retention;
    bool checkAfterBackup = false;

    bool isBackup() const { return mode == QStringLiteral("backup"); }
    bool isValid() const { return !name.trimmed().isEmpty() && !sources.isEmpty(); }
};

/// A stored schedule together with its scheduler bookkeeping.
struct ScheduleRecord {
    sched::ScheduleSpec spec;
    sched::ScheduleState state;
};

/// Counters persisted on a job run.
struct RunCounters {
    qint64 filesTotal = 0;
    qint64 filesTransferred = 0;
    qint64 filesSkipped = 0;
    qint64 filesFailed = 0;
    qint64 bytesTotal = 0;
    qint64 bytesTransferred = 0;

    /// Files examined so far (transferred + skipped + failed). Not persisted:
    /// it is derivable from the other counters.
    qint64 filesProcessed = 0;

    qint64 filesHandled() const { return filesTransferred + filesSkipped + filesFailed; }
};

/// One row of a run's file manifest.
struct ManifestRecord {
    QString relativePath; ///< path relative to the destination root, including the source label
    QString sourcePath;   ///< absolute local path the entry was read from
    qint64 sizeBytes = 0;
    QString modifiedUtc;
    QString sha256;
    QString state;      ///< pending | transferred | skipped | failed | verified
    QString skipReason;
};

/// Snapshot of a previously recorded manifest entry, used for incremental runs.
struct ManifestSnapshot {
    qint64 sizeBytes = 0;
    QString modifiedUtc;
    QString sha256;
};

/// Outcome of a single stored run, used by history views.
struct RunSummary {
    qint64 id = 0;
    qint64 jobId = 0;
    QString jobName;
    qint64 runNumber = 0;
    QString trigger;
    QString status;
    QString startedAt;
    QString finishedAt;
    RunCounters counters;
    QString errorCode;
    QString errorMessage;
};

/// Persistence for archive jobs, storage providers and job runs.
///
/// Every write is a single transaction. Reads never mutate state. The store
/// does not decide policy: it validates identity and constraints only.
class JobStore {
public:
    explicit JobStore(db::Database& database);

    // ---- storage providers -------------------------------------------------
    Result<QVector<StorageProviderRecord>> listProviders() const;
    Result<StorageProviderRecord> getProvider(qint64 id) const;
    Result<qint64> upsertLocalProvider(const QString& name, const QString& rootPath);
    Result<qint64> upsertRcloneProvider(const QString& name, const QString& rcloneRemote,
                                        const QString& basePath);
    Status updateProviderStatus(qint64 id, const QString& status, const QString& detail = {});
    /// Refuses to delete a provider that a job still uses.
    Status deleteProvider(qint64 id);

    // ---- jobs --------------------------------------------------------------
    Result<QVector<ArchiveJob>> listJobs() const;
    Result<ArchiveJob> getJob(qint64 id) const;
    Result<qint64> createJob(const ArchiveJob& job);
    Status updateJob(const ArchiveJob& job);
    /// Refuses to delete a job that currently has a running execution.
    Status deleteJob(qint64 id);

    // ---- runs --------------------------------------------------------------
    /// Starts a run. Fails with ErrorCode::AlreadyExists when the job is
    /// already running (enforced by a partial unique index).
    Result<qint64> beginRun(qint64 jobId, const QString& trigger);
    Status updateRunCounters(qint64 runId, const RunCounters& counters);
    Status finishRun(qint64 runId, const QString& status, const RunCounters& counters,
                     const std::optional<Error>& error = std::nullopt);

    /// Marks runs owned by a dead process on this host as 'interrupted'.
    /// Returns the number of recovered runs.
    Result<int> recoverStaleRuns();

    Result<QVector<RunSummary>> listRuns(int limit, std::optional<qint64> jobId = std::nullopt) const;
    Result<RunSummary> getRun(qint64 runId) const;
    Result<bool> isJobRunning(qint64 jobId) const;

    // ---- Restic snapshots --------------------------------------------------
    struct SnapshotRecord {
        qint64 id = 0;
        QString snapshotId;
        QString hostname;
        QString repositoryPath;
        QString createdAt;
        QStringList tags;
        qint64 sizeBytes = -1;
        int filesProcessed = -1;
    };

    /// Records a snapshot produced by a backup run (idempotent per job+snapshot).
    Status recordSnapshot(qint64 jobId, qint64 runId, const SnapshotRecord& snapshot);
    Result<QVector<SnapshotRecord>> listSnapshots(qint64 jobId, int limit = 200) const;
    Result<QVector<SnapshotRecord>> allSnapshots(int limit = 200) const;

    // ---- file manifest -----------------------------------------------------
    /// Manifest of the most recent successful run of the job (empty when there
    /// is none). This is what makes a second run incremental.
    Result<QMap<QString, ManifestSnapshot>> lastSuccessfulManifest(qint64 jobId) const;

    /// Manifest of the newest run that produced entries, for browsing and
    /// restore planning.
    Result<QVector<ManifestRecord>> latestManifest(qint64 jobId) const;

    Status recordManifestEntry(qint64 jobId, qint64 runId, const ManifestRecord& record);
    Status recordArchiveObject(qint64 jobId, qint64 runId, const QString& remotePath,
                               qint64 sizeBytes, const QString& sha256, const QString& state);

    // ---- schedules ---------------------------------------------------------
    /// Stored schedule for a job; std::nullopt when the job has none.
    Result<std::optional<ScheduleRecord>> getSchedule(qint64 jobId) const;

    /// Creates or replaces the schedule of a job.
    Status upsertSchedule(qint64 jobId, const sched::ScheduleSpec& spec);
    Status deleteSchedule(qint64 jobId);

    /// Persists the scheduler's bookkeeping (next slot, retry state, last run).
    Status updateScheduleState(qint64 jobId, const sched::ScheduleState& state);

    /// Every job with a recurring schedule, for the agent's tick.
    Result<QVector<QPair<qint64, ScheduleRecord>>> listSchedules() const;

    /// Jobs ordered by their next run, for the dashboard.
    Result<QVector<QPair<qint64, QDateTime>>> upcomingRuns(int limit) const;

    // ---- events ------------------------------------------------------------
    Status appendJobEvent(qint64 jobId, std::optional<qint64> runId, LogLevel level,
                          const QString& category, const QString& message,
                          const QJsonObject& data = {});

private:
    db::Database& database_;
};

} // namespace nesva::jobs
