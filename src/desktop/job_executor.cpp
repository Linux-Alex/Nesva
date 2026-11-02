// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "job_executor.hpp"

#include <QtCore/QDir>
#include <QtCore/QThread>

#include <utility>

#include "app_context.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/core/paths.hpp"
#include "nesva/db/database.hpp"
#include "nesva/jobs/job_store.hpp"

namespace nesva::ui {

/// Thread that owns one archive execution.
class JobExecutor::Worker final : public QThread {
    Q_OBJECT

public:
    Worker(QString databasePath, jobs::ArchiveJob job,
           std::optional<jobs::StorageProviderRecord> provider, jobs::RunOptions options,
           QString rcloneExecutable)
        : databasePath_(std::move(databasePath)),
          job_(std::move(job)),
          provider_(std::move(provider)),
          options_(options),
          rcloneExecutable_(std::move(rcloneExecutable)) {}

    void requestCancel() { cancellation_.cancel(); }

    jobs::RunReport report() const { return report_; }

signals:
    void progressChanged(const nesva::jobs::RunProgress& progress);
    void completed(const nesva::jobs::RunReport& report);

protected:
    void run() override {
        // A worker thread must not share the GUI thread's QSqlDatabase.
        db::Database database;
        const Status opened = database.open(databasePath_);
        if (!opened) {
            jobs::RunReport report;
            report.jobId = job_.id;
            report.jobName = job_.name;
            report.status = QStringLiteral("failed");
            report.errors.append(QString::fromStdString(opened.error().toDisplayString()));
            report_ = report;
            emit completed(report_);
            return;
        }

        jobs::ArchiveRunner runner(database, [this](const jobs::RunProgress& progress) {
            emit progressChanged(progress);
        });
        report_ = runner.run(job_, provider_, options_, cancellation_, QStringLiteral("manual"),
                             rcloneExecutable_);
        emit completed(report_);
    }

private:
    QString databasePath_;
    jobs::ArchiveJob job_;
    std::optional<jobs::StorageProviderRecord> provider_;
    jobs::RunOptions options_;
    QString rcloneExecutable_;
    CancellationToken cancellation_;
    jobs::RunReport report_;
};

JobExecutor::JobExecutor(AppContext& context, QObject* parent) : QObject(parent), context_(context) {
    qRegisterMetaType<nesva::jobs::RunProgress>("nesva::jobs::RunProgress");
    qRegisterMetaType<nesva::jobs::RunReport>("nesva::jobs::RunReport");
}

JobExecutor::~JobExecutor() {
    waitForFinished();
}

bool JobExecutor::isRunning() const {
    return worker_ != nullptr;
}

qint64 JobExecutor::runningJobId() const {
    return runningJobId_;
}

QString JobExecutor::runningJobName() const {
    return runningJobName_;
}

Status JobExecutor::startArchive(qint64 jobId, bool dryRun) {
    if (worker_) {
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
        Result<jobs::StorageProviderRecord> loaded = store.getProvider(*job.value().storageProviderId);
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
    if (worker_) {
        return makeError(ErrorCode::AlreadyExists, "another job is already running",
                         runningJobName_.toStdString());
    }

    const QString rcloneExecutable =
        context_.settings()
            .getString(QString::fromUtf8(config::keys::kRcloneBinaryPath), QStringLiteral("rclone"))
            .valueOr(QStringLiteral("rclone"));

    worker_ = std::make_unique<Worker>(Paths::databasePath(), job, std::move(provider), options,
                                       rcloneExecutable);
    runningJobId_ = job.id;
    runningJobName_ = job.name;

    connect(worker_.get(), &Worker::progressChanged, this,
            [this](const jobs::RunProgress& progress) { emit progressChanged(runningJobId_, progress); },
            Qt::QueuedConnection);
    connect(worker_.get(), &Worker::completed, this,
            [this](const jobs::RunReport& report) {
                const qint64 jobIdThatFinished = runningJobId_;
                const QString name = runningJobName_;
                runningJobId_ = 0;
                runningJobName_.clear();
                // The worker deletes itself after the thread has finished.
                if (worker_) {
                    worker_->wait();
                    worker_.reset();
                }
                context_.recordEvent(report.succeeded() ? LogLevel::Info : LogLevel::Warn,
                                     QStringLiteral("archive"),
                                     QStringLiteral("Archive run finished for '%1': %2")
                                         .arg(name, report.status));
                emit finished(jobIdThatFinished, report);
            },
            Qt::QueuedConnection);

    emit started(job.id, job.name);
    worker_->start();
    return Status();
}

void JobExecutor::cancel() {
    if (worker_) {
        worker_->requestCancel();
        emit progressChanged(runningJobId_, jobs::RunProgress{
                                                jobs::RunPhase::Finalising, QString(), 0, 0, 0, 0, 0,
                                                0, 0, QStringLiteral("Cancelling...")});
    }
}

void JobExecutor::waitForFinished(int timeoutMs) {
    if (!worker_) {
        return;
    }
    worker_->requestCancel();
    if (!worker_->wait(timeoutMs)) {
        logWarn("ui.executor", QStringLiteral("worker did not stop in time"));
    }
    worker_.reset();
    runningJobId_ = 0;
    runningJobName_.clear();
}

} // namespace nesva::ui

#include "job_executor.moc"
