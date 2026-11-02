// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>
#include <optional>

#include "nesva/core/result.hpp"
#include "nesva/jobs/archive_runner.hpp"

namespace nesva::jobs {

/// Runs one archive job on a worker thread.
///
/// The worker opens its own SQLite connection (Qt connections are thread
/// affine), so the caller's connection stays readable while the transfer runs.
/// Cancellation is an atomic token, which works even though the worker's event
/// loop is blocked for the duration of the run.
///
/// Used by the desktop application and by nesva-agent.
class JobWorker : public QObject {
    Q_OBJECT

public:
    explicit JobWorker(QString databasePath, QObject* parent = nullptr);
    ~JobWorker() override;

    bool isRunning() const;
    /// Identifier of the job currently executing, or 0.
    qint64 runningJobId() const { return runningJobId_; }

    /// Starts a run. Fails with AlreadyExists when a run is already active.
    Status start(const ArchiveJob& job, std::optional<StorageProviderRecord> provider,
                 const RunOptions& options, const QString& rcloneExecutable,
                 const QString& trigger);

    void cancel();
    void waitForFinished(int timeoutMs = 30000);

signals:
    void progressChanged(const nesva::jobs::RunProgress& progress);
    void completed(const nesva::jobs::RunReport& report);

private:
    class Thread;

    QString databasePath_;
    std::unique_ptr<Thread> thread_;
    qint64 runningJobId_ = 0;
};

} // namespace nesva::jobs
