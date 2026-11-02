// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <optional>

#include "nesva/core/result.hpp"
#include "nesva/jobs/archive_runner.hpp"
#include "nesva/jobs/backup_runner.hpp"
#include "nesva/jobs/job_worker.hpp"

namespace nesva::ui {

class AppContext;

/// Runs jobs on a worker thread so the window stays responsive.
///
/// The worker opens its own SQLite connection (Qt connections are thread
/// affine). Archive jobs run the manifest-based archive engine; backup jobs
/// drive Restic through the same job lifecycle (counters, events, snapshots).
class JobExecutor : public QObject {
    Q_OBJECT

public:
    explicit JobExecutor(AppContext& context, QObject* parent = nullptr);
    ~JobExecutor() override;

    bool isRunning() const;
    qint64 runningJobId() const { return runningJobId_; }
    QString runningJobName() const { return runningJobName_; }

    /// Starts a stored job (archive or backup). Fails when a run is active.
    Status startRun(qint64 jobId, bool dryRun = false);

    /// Runs an arbitrary (possibly unsaved) job definition, used by the job
    /// editor's dry-run preview.
    Status startPreview(const jobs::ArchiveJob& job,
                        std::optional<jobs::StorageProviderRecord> provider);

    void cancel();
    void waitForFinished(int timeoutMs = 30000);

signals:
    void started(qint64 jobId, const QString& jobName);
    void progressChanged(qint64 jobId, const nesva::jobs::RunProgress& progress);
    void finished(qint64 jobId, const nesva::jobs::RunReport& report);

private:
    Status startWithJob(const jobs::ArchiveJob& job,
                        std::optional<jobs::StorageProviderRecord> provider,
                        const jobs::RunOptions& options);

    AppContext& context_;
    jobs::JobWorker worker_;
    qint64 runningJobId_ = 0;
    QString runningJobName_;
};

} // namespace nesva::ui
