// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>
#include <optional>

#include "nesva/core/result.hpp"
#include "nesva/jobs/archive_runner.hpp"

namespace nesva::ui {

class AppContext;

/// Runs archive jobs on a worker thread so the window stays responsive.
///
/// The worker opens its own SQLite connection (Qt connections are
/// thread-affine), so the GUI thread keeps reading while a transfer writes.
/// Cancellation is a thread-safe token: the GUI can cancel even though the
/// worker's event loop is blocked inside the run.
class JobExecutor : public QObject {
    Q_OBJECT

public:
    explicit JobExecutor(AppContext& context, QObject* parent = nullptr);
    ~JobExecutor() override;

    bool isRunning() const;
    qint64 runningJobId() const;
    QString runningJobName() const;

    /// Starts a job stored in the database. Fails when a run is already in progress.
    Status startArchive(qint64 jobId, bool dryRun = false);

    /// Runs an arbitrary (possibly unsaved) job definition, normally as a dry
    /// run from the job editor preview.
    Status startPreview(const jobs::ArchiveJob& job,
                        std::optional<jobs::StorageProviderRecord> provider);

    void cancel();
    /// Blocks until the worker has finished; used when the window closes.
    void waitForFinished(int timeoutMs = 30000);

signals:
    void started(qint64 jobId, const QString& jobName);
    void progressChanged(qint64 jobId, const nesva::jobs::RunProgress& progress);
    void finished(qint64 jobId, const nesva::jobs::RunReport& report);

private:
    class Worker;

    Status startWithJob(const jobs::ArchiveJob& job,
                        std::optional<jobs::StorageProviderRecord> provider,
                        const jobs::RunOptions& options);

    AppContext& context_;
    std::unique_ptr<Worker> worker_;
    qint64 runningJobId_ = 0;
    QString runningJobName_;
};

} // namespace nesva::ui
