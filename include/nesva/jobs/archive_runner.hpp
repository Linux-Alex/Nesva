// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <functional>
#include <memory>
#include <optional>

#include "nesva/core/cancellation.hpp"
#include "nesva/core/result.hpp"
#include "nesva/jobs/job_store.hpp"
#include "nesva/providers/provider_factory.hpp"
#include "nesva/providers/storage_provider.hpp"
#include "nesva/scan/file_scanner.hpp"

namespace nesva::db {
class Database;
}

namespace nesva::jobs {

enum class RunPhase {
    Idle,
    Preparing,
    Scanning,
    Transferring,
    Finalising,
    Done,
};

/// Live progress of an archive run. Delivered to the UI and to the CLI.
struct RunProgress {
    RunPhase phase = RunPhase::Idle;
    QString currentFile;
    qint64 filesTotal = 0;
    qint64 filesProcessed = 0;
    qint64 filesTransferred = 0;
    qint64 filesSkipped = 0;
    qint64 filesFailed = 0;
    qint64 bytesTotal = 0;
    qint64 bytesTransferred = 0;
    QString message;
};

struct RunOptions {
    /// Analyse and report without uploading anything.
    bool dryRun = false;
    /// Ignore the previous manifest and transfer everything again.
    bool forceFullTransfer = false;
    int maxAttempts = 3;
    int retryBackoffMs = 400;
    /// Upload a whole directory per rclone invocation instead of one process per
    /// file. Much faster for many small files; on by default for rclone
    /// destinations, disabled with "jobs run --no-batch".
    bool batchUpload = true;
};

struct RunReport {
    qint64 runId = 0;
    qint64 jobId = 0;
    QString jobName;
    QString status = QStringLiteral("failed");
    RunCounters counters;
    int uploads = 0;
    int skipped = 0;
    int failures = 0;
    bool dryRun = false;
    QStringList errors;
    QVector<scan::ScanSkip> skippedEntries;

    bool succeeded() const { return status == QStringLiteral("succeeded"); }
};

using RunProgressCallback = std::function<void(const RunProgress&)>;

/// Result of resolving a job's destination into a provider configuration.
struct ResolvedDestination {
    providers::ProviderSelection selection;
    std::unique_ptr<providers::StorageProvider> provider;
    /// Absolute path of the destination when it is local; used to exclude the
    /// destination from the scan so a job can never archive its own output.
    QString localExclusionRoot;
};

/// Builds the provider for a job. Failures are configuration problems
/// (missing destination, unknown provider) reported before any transfer.
Result<ResolvedDestination> resolveDestination(const ArchiveJob& job,
                                              const std::optional<StorageProviderRecord>& provider,
                                              const QString& rcloneExecutable = {});

/// Executes archive jobs.
///
/// Safety properties:
///  * source files are only ever read - nothing is deleted, moved or rewritten;
///  * the destination is excluded from the scan, so a job cannot recurse into
///    its own output;
///  * files unchanged since the last successful run are skipped, and the reason
///    is recorded in the manifest;
///  * a failed file does not abort the whole run unless the destination itself
///    becomes unusable; the run then finishes as 'failed' with every error kept;
///  * cancellation stops between files and records the run as 'cancelled';
///  * duplicate concurrent runs are rejected by the database, not by the UI.
class ArchiveRunner {
public:
    ArchiveRunner(db::Database& database, RunProgressCallback progress = {});

    /// Runs the job. Never throws; every failure is part of the report.
    RunReport run(const ArchiveJob& job, const std::optional<StorageProviderRecord>& provider,
                  const RunOptions& options, const CancellationToken& cancellation,
                  const QString& trigger = QStringLiteral("manual"),
                  const QString& rcloneExecutable = {});

private:
    void emitProgress(const RunProgress& progress) const;

    db::Database& database_;
    JobStore store_;
    RunProgressCallback progress_;
};

} // namespace nesva::jobs
