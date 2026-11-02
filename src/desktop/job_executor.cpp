// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "job_executor.hpp"

#include <QtCore/QDir>

#include <utility>

#include "app_context.hpp"
#include "nesva/config/settings_store.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/core/paths.hpp"
#include "nesva/db/database.hpp"
#include "nesva/jobs/job_store.hpp"

namespace nesva::ui {

JobExecutor::JobExecutor(AppContext& context, QObject* parent)
    : QObject(parent), context_(context), worker_(Paths::databasePath(), this) {
    qRegisterMetaType<nesva::jobs::RunProgress>("nesva::jobs::RunProgress");
    qRegisterMetaType<nesva::jobs::RunReport>("nesva::jobs::RunReport");

    connect(&worker_, &jobs::JobWorker::progressChanged, this,
            [this](const jobs::RunProgress& progress) {
                emit progressChanged(runningJobId_, progress);
            });
    connect(&worker_, &jobs::JobWorker::completed, this, [this](const jobs::RunReport& report) {
        const qint64 jobId = runningJobId_;
        const QString name = runningJobName_;
        runningJobId_ = 0;
        runningJobName_.clear();
        context_.recordEvent(report.succeeded() ? LogLevel::Info : LogLevel::Warn,
                             QStringLiteral("jobs"),
                             QStringLiteral("Run finished for '%1': %2").arg(name, report.status));
        emit finished(jobId, report);
    });
}

JobExecutor::~JobExecutor() {
    waitForFinished();
}

bool JobExecutor::isRunning() const {
    return worker_.isRunning();
}

Status JobExecutor::startRun(qint64 jobId, bool dryRun) {
    if (worker_.isRunning()) {
        return makeError(ErrorCode::AlreadyExists, "another job is already running",
                         runningJobName_.toStdString());
    }

    jobs::JobStore store(context_.database());
    Result<jobs::ArchiveJob> job = store.getJob(jobId);
    if (!job) {
        return job.error();
    }
    std::optional<jobs::StorageProviderRecord> provider;
    if (job.value().storageProviderId) {
        Result<jobs::StorageProviderRecord> loaded =
            store.getProvider(*job.value().storageProviderId);
        if (!loaded) {
            return loaded.error();
        }
        provider = loaded.value();
    }

    jobs::RunOptions options;
    options.dryRun = dryRun;
    return startWithJob(job.value(), std::move(provider), options);
}

Status JobExecutor::startPreview(const jobs::ArchiveJob& job,
                                 std::optional<jobs::StorageProviderRecord> provider) {
    jobs::RunOptions options;
    options.dryRun = true;
    return startWithJob(job, std::move(provider), options);
}

Status JobExecutor::startWithJob(const jobs::ArchiveJob& job,
                                 std::optional<jobs::StorageProviderRecord> provider,
                                 const jobs::RunOptions& options) {
    if (worker_.isRunning()) {
        return makeError(ErrorCode::AlreadyExists, "another job is already running",
                         runningJobName_.toStdString());
    }

    config::SettingsStore settings(context_.database());
    const QString rcloneExecutable =
        settings.getString(QString::fromUtf8(config::keys::kRcloneBinaryPath), QStringLiteral("rclone"))
            .valueOr(QStringLiteral("rclone"));
    const QString resticExecutable =
        settings.getString(QString::fromUtf8(config::keys::kResticBinaryPath), QStringLiteral("restic"))
            .valueOr(QStringLiteral("restic"));

    const qint64 jobId = job.id;
    const QString jobName = job.name;
    runningJobId_ = jobId;
    runningJobName_ = jobName;
    const Status runStarted = worker_.start(job, std::move(provider), options, rcloneExecutable,
                                            QStringLiteral("desktop"), resticExecutable,
                                            jobs::passwordOptionsFromSettings(context_.database()));
    if (!runStarted.ok()) {
        runningJobId_ = 0;
        runningJobName_.clear();
        return runStarted;
    }
    emit started(jobId, jobName);
    return Status();
}

void JobExecutor::cancel() {
    if (worker_.isRunning()) {
        worker_.cancel();
        emit progressChanged(runningJobId_, jobs::RunProgress{
                                                jobs::RunPhase::Finalising, QString(), 0, 0, 0, 0, 0,
                                                0, 0, QStringLiteral("Cancelling...")});
    }
}

void JobExecutor::waitForFinished(int timeoutMs) {
    worker_.waitForFinished(timeoutMs);
    runningJobId_ = 0;
    runningJobName_.clear();
}

} // namespace nesva::ui
