// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "agent_service.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QLockFile>
#include <QtCore/QTimer>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

#include <algorithm>
#include <utility>

#include "nesva/config/settings_store.hpp"
#include "nesva/core/app_info.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/core/paths.hpp"
#include "nesva/db/database.hpp"
#include "nesva/jobs/job_worker.hpp"

namespace nesva::agent {
namespace {

constexpr int kStopGraceMs = 15000;

QString describeReport(const jobs::RunReport& report) {
    return QStringLiteral("%1: %2 transferred, %3 unchanged, %4 failed")
        .arg(report.status)
        .arg(report.counters.filesTransferred)
        .arg(report.counters.filesSkipped)
        .arg(report.counters.filesFailed);
}

} // namespace

AgentService::AgentService(AgentOptions options, QObject* parent)
    : QObject(parent), options_(std::move(options)) {}

AgentService::~AgentService() {
    if (worker_) {
        worker_->waitForFinished(5000);
        worker_.reset();
    }
    if (server_ != nullptr) {
        server_->close();
    }
    QLocalServer::removeServer(socketName());
}

Status AgentService::initialise() {
    if (!options_.dataRootOverride.isEmpty()) {
        Paths::setDataRootOverride(options_.dataRootOverride);
    }
    const Status directories = Paths::ensureBaseDirectories();
    if (!directories) {
        return directories;
    }

    // One agent per data directory. The lock file is separate from the socket so
    // a crashed process cannot block the next start (QStaleLockTime).
    auto lockFile = std::make_unique<QLockFile>(lockFilePath());
    lockFile->setStaleLockTime(0);
    if (!lockFile->tryLock(100)) {
        return makeError(ErrorCode::AlreadyExists, "another nesva-agent is already running",
                         lockFilePath().toStdString());
    }
    // Keep the lock alive for the whole process lifetime by leaking it into a
    // static; the OS releases it when the process exits.
    static std::unique_ptr<QLockFile> g_lock = std::move(lockFile);

    LoggingOptions logging;
    logging.logDirectory = Paths::logsDirectory();
    logging.fileName = QStringLiteral("nesva-agent.log");
    logging.fileLevel = LogLevel::Debug;
    logging.consoleEnabled = options_.consoleLogging;
    logging.consoleLevel = LogLevel::Info;
    logging.consoleToStandardError = true;
    const Result<QString> logFile = initialiseLogging(logging);
    if (!logFile) {
        return logFile.error();
    }

    database_ = std::make_unique<db::Database>();
    const Status opened = database_->open(Paths::databasePath());
    if (!opened) {
        return opened;
    }
    const Status migrated = database_->migrate();
    if (!migrated) {
        return migrated;
    }
    store_ = std::make_unique<jobs::JobStore>(*database_);

    // Runs left behind by a crashed agent are recovered before scheduling
    // resumes, so a stale row cannot block a job forever.
    Result<int> recovered = store_->recoverStaleRuns();
    if (recovered && recovered.value() > 0) {
        logWarn("agent", QStringLiteral("recovered interrupted runs"),
                QJsonObject{{QStringLiteral("count"), recovered.value()}});
    }

    worker_ = std::make_unique<jobs::JobWorker>(Paths::databasePath(), this);
    connect(worker_.get(), &jobs::JobWorker::completed, this,
            [this](const jobs::RunReport& report) { onJobCompleted(report.jobId, report); });

    QLocalServer::removeServer(socketName());
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(socketName())) {
        logWarn("agent", QStringLiteral("cannot listen on the agent socket"),
                QJsonObject{{QStringLiteral("socket"), socketName()},
                            {QStringLiteral("error"), server_->errorString()}});
    } else {
        connect(server_, &QLocalServer::newConnection, this, [this] {
            while (QLocalSocket* socket = server_->nextPendingConnection()) {
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);

                // Requests may already be buffered by the time the connection is
                // accepted (a local client writes immediately), and readyRead is
                // only emitted for *new* data. Drain first, then keep listening.
                auto processPendingData = [this, socket] {
                    while (socket->canReadLine()) {
                        const QByteArray line = socket->readLine().trimmed();
                        if (line.isEmpty()) {
                            continue;
                        }
                        const QJsonDocument document = QJsonDocument::fromJson(line);
                        if (!document.isObject()) {
                            sendError(socket, Error(ErrorCode::InvalidArgument,
                                                    "request must be a JSON object"));
                            continue;
                        }
                        handleRequest(socket, document.object());
                    }
                };
                connect(socket, &QLocalSocket::readyRead, this, processPendingData);
                processPendingData();
            }
        });
    }

    startedAtUtc_ = QDateTime::currentDateTimeUtc();
    initialised_ = true;
    lastMessage_ = QStringLiteral("agent started");

    tickTimer_ = new QTimer(this);
    connect(tickTimer_, &QTimer::timeout, this, &AgentService::tick);
    tickTimer_->start(options_.tickIntervalMs);

    logInfo("agent", QStringLiteral("agent started"),
            QJsonObject{{QStringLiteral("version"), ApplicationInfo::version()},
                        {QStringLiteral("database"), Paths::databasePath()},
                        {QStringLiteral("socket"), socketName()}});
    writeHeartbeatNow();
    return Status();
}

void AgentService::tick() {
    if (!initialised_) {
        return;
    }
    if (stopping_) {
        return;
    }

    // Startup-triggered jobs run once, right after the agent comes up.
    if (store_ && worker_ && !worker_->isRunning()) {
        startNextDueJob();
    }
    writeHeartbeatNow();
}

void AgentService::startNextDueJob() {
    Result<QVector<QPair<qint64, jobs::ScheduleRecord>>> schedules = store_->listSchedules();
    if (!schedules) {
        logWarn("agent", QStringLiteral("cannot read schedules"),
                QJsonObject{{QStringLiteral("error"),
                             QString::fromStdString(schedules.error().toDisplayString())}});
        return;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    scheduledJobs_ = static_cast<int>(schedules.value().size());
    nextRunUtc_ = QDateTime();

    // The earliest pending slot is reported to the UI.
    for (const auto& entry : schedules.value()) {
        const std::optional<QDateTime>& slot = entry.second.state.nextRunUtc;
        if (slot.has_value() && (!nextRunUtc_.isValid() || *slot < nextRunUtc_)) {
            nextRunUtc_ = *slot;
        }
    }

    for (const auto& entry : schedules.value()) {
        const qint64 jobId = entry.first;
        const jobs::ScheduleRecord& record = entry.second;

        const sched::DueDecision decision =
            sched::evaluateDue(record.spec, record.state, now, startedAtUtc_);

        if (!decision.shouldRun()) {
            // Persist a freshly computed slot (first evaluation) so the UI shows
            // the next run immediately and the next tick has a stable value.
            if (decision.nextRunUtc.isValid() && decision.nextRunUtc != record.state.nextRunUtc) {
                sched::ScheduleState state = record.state;
                state.nextRunUtc = decision.nextRunUtc;
                store_->updateScheduleState(jobId, state);
            }
            continue;
        }

        Result<jobs::ArchiveJob> job = store_->getJob(jobId);
        if (!job) {
            logWarn("agent", QStringLiteral("cannot load the job for a due schedule"),
                    QJsonObject{{QStringLiteral("job_id"), jobId},
                                {QStringLiteral("error"),
                                 QString::fromStdString(job.error().toDisplayString())}});
            continue;
        }

        jobs::RunOptions runOptions;
        const Status started = worker_->start(job.value(), providerFor(job.value()), runOptions,
                                              rcloneExecutable(), sched::toToken(decision.reason));
        if (!started) {
            // Another process (for example the desktop application) holds the
            // single-flight guard. Try again on the next tick.
            logDebug("agent", QStringLiteral("job could not start now"),
                     QJsonObject{{QStringLiteral("job_id"), jobId},
                                 {QStringLiteral("error"), QString::fromStdString(started.error().message())}});
            continue;
        }

        runningSpec_ = record.spec;
        runningAttemptsUsed_ = decision.reason == sched::DueReason::Retry ? decision.retryAttempt : 0;
        lastMessage_ = QStringLiteral("running '%1' (%2)").arg(job.value().name,
                                                               sched::toToken(decision.reason));

        // Persist the next scheduled slot immediately: a missed catch-up must
        // not fire again while the catch-up run is still going.
        sched::ScheduleState state = record.state;
        state.nextRunUtc = decision.nextRunUtc;
        if (decision.reason != sched::DueReason::Retry) {
            state.retryAttemptCount = 0;
            state.retryNextUtc = std::nullopt;
        }
        store_->updateScheduleState(jobId, state);

        if (record.spec.maxDurationSeconds > 0) {
            if (deadlineTimer_ == nullptr) {
                deadlineTimer_ = new QTimer(this);
                deadlineTimer_->setSingleShot(true);
                connect(deadlineTimer_, &QTimer::timeout, this, [this] {
                    if (worker_ && worker_->isRunning()) {
                        lastMessage_ = QStringLiteral("cancelled after exceeding the maximum duration");
                        worker_->cancel();
                    }
                });
            }
            deadlineTimer_->start(record.spec.maxDurationSeconds * 1000);
        } else if (deadlineTimer_ != nullptr) {
            deadlineTimer_->stop();
        }

        store_->appendJobEvent(jobId, std::nullopt, LogLevel::Info, QStringLiteral("agent"),
                               QStringLiteral("Background run started (%1)")
                                   .arg(sched::toToken(decision.reason)),
                               QJsonObject{{QStringLiteral("slot"),
                                            record.state.nextRunUtc.has_value()
                                                ? record.state.nextRunUtc->toString(Qt::ISODate)
                                                : QString()},
                                           {QStringLiteral("explanation"), decision.explanation}});
        writeHeartbeatNow();
        return; // one job at a time
    }
}

void AgentService::onJobCompleted(qint64 jobId, const jobs::RunReport& report) {
    if (deadlineTimer_ != nullptr) {
        deadlineTimer_->stop();
    }

    const QString summary = describeReport(report);
    lastMessage_ = summary;
    logInfo("agent", QStringLiteral("background run finished"),
            QJsonObject{{QStringLiteral("job_id"), jobId},
                        {QStringLiteral("status"), report.status},
                        {QStringLiteral("transferred"), report.counters.filesTransferred},
                        {QStringLiteral("failed"), report.counters.filesFailed}});

    if (runningSpec_.has_value() && store_) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const bool succeeded = report.status == QStringLiteral("succeeded");
        const sched::OutcomePlan plan =
            sched::planAfterRun(runningSpec_.value(), now, succeeded, runningAttemptsUsed_);

        sched::ScheduleState state;
        state.lastRunUtc = now;
        state.nextRunUtc = plan.nextRunUtc;
        state.retryAttemptCount = plan.retryAttemptCount;
        state.retryNextUtc = plan.retryNextUtc;
        store_->updateScheduleState(jobId, state);

        if (plan.willRetry) {
            store_->appendJobEvent(
                jobId, std::nullopt, LogLevel::Warn, QStringLiteral("agent"),
                QStringLiteral("Run failed; retry scheduled in %1s (attempt %2 of %3)")
                    .arg(now.secsTo(plan.retryNextUtc.value_or(now)))
                    .arg(plan.retryAttemptCount)
                    .arg(runningSpec_->retryMaxAttempts + 1));
        }
    }
    runningSpec_.reset();
    runningAttemptsUsed_ = 0;

    emit jobFinished(jobId, report);
    writeHeartbeatNow();

    if (stopping_) {
        finishStop();
    }
}

void AgentService::requestStop() {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    lastMessage_ = QStringLiteral("stopping");
    writeHeartbeatNow();

    if (worker_ && worker_->isRunning()) {
        logInfo("agent", QStringLiteral("stopping after the current run finishes"));
        // Cancel only when the caller asked for an immediate stop; graceful
        // shutdown lets the current file complete.
    } else {
        finishStop();
    }
}

void AgentService::finishStop() {
    if (deadlineTimer_ != nullptr) {
        deadlineTimer_->stop();
    }
    if (tickTimer_ != nullptr) {
        tickTimer_->stop();
    }
    if (server_ != nullptr) {
        server_->close();
        QLocalServer::removeServer(socketName());
    }
    writeHeartbeatNow();

    AgentStatus finalStatus = status();
    finalStatus.running = false;
    finalStatus.state = QStringLiteral("stopped");
    finalStatus.runningJobId = 0;
    finalStatus.runningJobName.clear();
    finalStatus.lastMessage = QStringLiteral("agent stopped");
    finalStatus.updatedAtUtc = QDateTime::currentDateTimeUtc();
    writeHeartbeat(finalStatus);

    logInfo("agent", QStringLiteral("agent stopped"));
    emit stopped(0);
}

AgentStatus AgentService::status() const {
    AgentStatus status;
    status.running = true;
    status.pid = QCoreApplication::applicationPid();
    status.version = ApplicationInfo::version();
    status.startedAtUtc = startedAtUtc_;
    status.updatedAtUtc = QDateTime::currentDateTimeUtc();
    status.scheduledJobs = scheduledJobs_;
    status.nextRunUtc = nextRunUtc_;
    status.lastMessage = lastMessage_;

    if (stopping_) {
        status.state = QStringLiteral("stopping");
    } else if (worker_ && worker_->isRunning()) {
        status.state = QStringLiteral("running");
        status.runningJobId = worker_->runningJobId();
    } else {
        status.state = QStringLiteral("idle");
    }

    if (status.runningJobId > 0 && store_) {
        Result<jobs::ArchiveJob> job = store_->getJob(status.runningJobId);
        if (job) {
            status.runningJobName = job.value().name;
        }
    }
    return status;
}

void AgentService::writeHeartbeatNow() {
    if (!initialised_) {
        return;
    }
    const Result<QString> written = writeHeartbeat(status());
    if (!written) {
        logWarn("agent", QStringLiteral("cannot write the heartbeat file"),
                QJsonObject{{QStringLiteral("error"),
                             QString::fromStdString(written.error().toDisplayString())}});
    }
}

std::optional<jobs::StorageProviderRecord> AgentService::providerFor(const jobs::ArchiveJob& job) {
    if (!job.storageProviderId || !store_) {
        return std::nullopt;
    }
    Result<jobs::StorageProviderRecord> provider = store_->getProvider(*job.storageProviderId);
    if (!provider) {
        return std::nullopt;
    }
    return provider.value();
}

QString AgentService::rcloneExecutable() const {
    if (!database_) {
        return QStringLiteral("rclone");
    }
    // The agent reads the same settings table as the application, so a custom
    // rclone path configured in the GUI is honoured by background runs too.
    config::SettingsStore settings(*database_);
    const Result<QString> configured =
        settings.getString(QString::fromUtf8(config::keys::kRcloneBinaryPath));
    if (configured && !configured.value().trimmed().isEmpty()) {
        return configured.value();
    }
    return QStringLiteral("rclone");
}

void AgentService::handleRequest(QLocalSocket* socket, const QJsonObject& request) {
    const Result<Request> parsed = parseRequest(request);
    if (!parsed) {
        sendError(socket, parsed.error());
        return;
    }
    const Request command = parsed.value();

    if (command.command == QString::fromUtf8(command::kStatus)) {
        QJsonObject response = statusToJson(status());
        response.insert(QStringLiteral("ok"), true);
        sendResponse(socket, response);
        return;
    }

    if (command.command == QString::fromUtf8(command::kReload)) {
        scheduledJobs_ = 0;
        nextRunUtc_ = QDateTime();
        tick();
        QJsonObject response{{QStringLiteral("ok"), true},
                             {QStringLiteral("reloaded"), true}};
        sendResponse(socket, response);
        return;
    }

    if (command.command == QString::fromUtf8(command::kShutdown)) {
        sendResponse(socket, QJsonObject{{QStringLiteral("ok"), true},
                                         {QStringLiteral("stopping"), true}});
        requestStop();
        return;
    }

    if (command.command == QString::fromUtf8(command::kCancel)) {
        if (!worker_ || !worker_->isRunning() || worker_->runningJobId() != command.jobId) {
            sendError(socket, Error(ErrorCode::NotFound, "that job is not running"));
            return;
        }
        lastMessage_ = QStringLiteral("cancelling job %1 on request").arg(command.jobId);
        worker_->cancel();
        sendResponse(socket, QJsonObject{{QStringLiteral("ok"), true},
                                         {QStringLiteral("cancelling"), command.jobId}});
        return;
    }

    if (command.command == QString::fromUtf8(command::kRun)) {
        if (!worker_) {
            sendError(socket, Error(ErrorCode::Internal, "worker is not available"));
            return;
        }
        if (worker_->isRunning()) {
            sendError(socket, Error(ErrorCode::AlreadyExists,
                                    "the agent is already running a job",
                                    QString::number(worker_->runningJobId()).toStdString()));
            return;
        }
        Result<jobs::ArchiveJob> job = store_->getJob(command.jobId);
        if (!job) {
            sendError(socket, job.error());
            return;
        }
        jobs::RunOptions options;
        options.dryRun = command.dryRun;
        const Status started = worker_->start(job.value(), providerFor(job.value()), options,
                                              rcloneExecutable(), command.trigger);
        if (!started) {
            sendError(socket, started.error());
            return;
        }
        runningSpec_ = std::nullopt; // manual runs do not advance the schedule
        runningAttemptsUsed_ = 0;
        lastMessage_ = QStringLiteral("running '%1' (requested)").arg(job.value().name);
        writeHeartbeatNow();
        sendResponse(socket, QJsonObject{{QStringLiteral("ok"), true},
                                         {QStringLiteral("started"), command.jobId}});
        return;
    }

    sendError(socket, Error(ErrorCode::InvalidArgument, "unhandled command"));
}

void AgentService::sendResponse(QLocalSocket* socket, const QJsonObject& response) {
    if (socket == nullptr) {
        return;
    }
    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    socket->write("\n");
    socket->flush();
}

void AgentService::sendError(QLocalSocket* socket, const Error& error) {
    QJsonObject errorObject;
    errorObject.insert(QStringLiteral("code"), QString::fromUtf8(toToken(error.code())));
    errorObject.insert(QStringLiteral("message"), QString::fromStdString(error.message()));
    if (!error.detail().empty()) {
        errorObject.insert(QStringLiteral("detail"), QString::fromStdString(error.detail()));
    }
    QJsonObject response;
    response.insert(QStringLiteral("ok"), false);
    response.insert(QStringLiteral("error"), errorObject);
    sendResponse(socket, response);
}

} // namespace nesva::agent
