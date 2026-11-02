// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>
#include <optional>

#include "nesva/agent/protocol.hpp"
#include "nesva/core/result.hpp"
#include "nesva/jobs/archive_runner.hpp"
#include "nesva/jobs/job_worker.hpp"
#include "nesva/jobs/job_store.hpp"
#include "nesva/scheduler/schedule.hpp"

class QLocalServer;
class QLocalSocket;
class QTimer;

namespace nesva::db {
class Database;
}

namespace nesva::agent {

struct AgentOptions {
    QString dataRootOverride;
    int tickIntervalMs = 20000;
    bool consoleLogging = false;
    /// Run one scheduling pass and exit once nothing is running (used by tests
    /// and by "run the agent once" from automation).
    bool runOnce = false;
};

/// The background service: schedules, executes and reports archive jobs
/// without the desktop application being open.
///
/// Responsibilities:
///  * one tick loop that evaluates schedules and starts at most one job at a
///    time (the database additionally forbids two concurrent runs per job);
///  * missed-run catch-up and persistent retry state;
///  * maximum run duration enforcement;
///  * a local socket so the desktop and CLI can query status, start, cancel or
///    stop it;
///  * a heartbeat file so the UI can show agent state even when disconnected;
///  * graceful shutdown that lets the current file finish without corrupting
///    the destination.
class AgentService final : public QObject {
    Q_OBJECT

public:
    explicit AgentService(AgentOptions options, QObject* parent = nullptr);
    ~AgentService() override;

    /// Opens the database, takes the single-instance lock, starts the IPC
    /// server and the tick timer.
    Status initialise();

    /// Starts the event-independent part: evaluates schedules once.
    void tick();

    /// Requests a graceful stop; the service finishes the current file, updates
    /// the schedule state and then emits stopped().
    void requestStop();
    bool isStopping() const { return stopping_; }

    AgentStatus status() const;
    void writeHeartbeatNow();

signals:
    void stopped(int exitCode);
    void jobFinished(qint64 jobId, const nesva::jobs::RunReport& report);

private:
    void startNextDueJob();
    void onJobCompleted(qint64 jobId, const nesva::jobs::RunReport& report);
    void finishStop();

    void handleRequest(QLocalSocket* socket, const QJsonObject& request);
    void sendResponse(QLocalSocket* socket, const QJsonObject& response);
    void sendError(QLocalSocket* socket, const Error& error);

    std::optional<jobs::StorageProviderRecord> providerFor(const jobs::ArchiveJob& job);
    QString rcloneExecutable() const;
    QString resticExecutable() const;

    AgentOptions options_;
    std::unique_ptr<db::Database> database_;
    std::unique_ptr<jobs::JobStore> store_;
    std::unique_ptr<jobs::JobWorker> worker_;
    QLocalServer* server_ = nullptr;
    QTimer* tickTimer_ = nullptr;
    QTimer* deadlineTimer_ = nullptr;

    QDateTime startedAtUtc_;
    QDateTime nextRunUtc_;
    int scheduledJobs_ = 0;
    bool stopping_ = false;
    bool initialised_ = false;
    QString lastMessage_;

    /// Schedule of the job currently executing, needed to plan the next slot.
    std::optional<sched::ScheduleSpec> runningSpec_;
    int runningAttemptsUsed_ = 0;
};

} // namespace nesva::agent
