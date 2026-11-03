// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/jobs/job_worker.hpp"

#include <QtCore/QThread>

#include <utility>

#include "nesva/db/database.hpp"
#include "nesva/jobs/job_store.hpp"
#include "nesva/jobs/mail_job_runner.hpp"

namespace nesva::jobs {

class JobWorker::Thread final : public QThread {
    Q_OBJECT

public:
    Thread(QString databasePath, ArchiveJob job, std::optional<StorageProviderRecord> provider,
           RunOptions options, QString rcloneExecutable, QString trigger,
           QString resticExecutable, PasswordOptions passwordOptions)
        : databasePath_(std::move(databasePath)),
          job_(std::move(job)),
          provider_(std::move(provider)),
          options_(options),
          rcloneExecutable_(std::move(rcloneExecutable)),
          trigger_(std::move(trigger)),
          resticExecutable_(std::move(resticExecutable)),
          passwordOptions_(std::move(passwordOptions)) {}

    void requestCancel() { cancellation_.cancel(); }
    RunReport report() const { return report_; }

signals:
    void progressChanged(const nesva::jobs::RunProgress& progress);
    void completed(const nesva::jobs::RunReport& report);

protected:
    void run() override {
        db::Database database;
        const Status opened = database.open(databasePath_);
        if (!opened) {
            RunReport failed;
            failed.jobId = job_.id;
            failed.jobName = job_.name;
            failed.status = QStringLiteral("failed");
            failed.errors.append(QString::fromStdString(opened.error().toDisplayString()));
            report_ = failed;
            emit completed(report_);
            return;
        }

        const auto progressCallback = [this](const RunProgress& progress) {
            emit progressChanged(progress);
        };

        if (job_.isBackup()) {
            BackupRunner runner(database, progressCallback);
            report_ = runner.run(job_, options_, cancellation_, trigger_, resticExecutable_,
                                 passwordOptions_);
        } else if (job_.isMail()) {
            MailJobRunner runner(database, progressCallback);
            report_ = runner.run(job_, options_, cancellation_, trigger_);
        } else {
            ArchiveRunner runner(database, progressCallback);
            report_ = runner.run(job_, provider_, options_, cancellation_, trigger_,
                                 rcloneExecutable_);
        }
        emit completed(report_);
    }

private:
    QString databasePath_;
    ArchiveJob job_;
    std::optional<StorageProviderRecord> provider_;
    RunOptions options_;
    QString rcloneExecutable_;
    QString trigger_;
    QString resticExecutable_;
    PasswordOptions passwordOptions_;
    CancellationToken cancellation_;
    RunReport report_;
};

JobWorker::JobWorker(QString databasePath, QObject* parent)
    : QObject(parent), databasePath_(std::move(databasePath)) {}

JobWorker::~JobWorker() {
    waitForFinished();
}

bool JobWorker::isRunning() const {
    return thread_ != nullptr;
}

Status JobWorker::start(const ArchiveJob& job, std::optional<StorageProviderRecord> provider,
                        const RunOptions& options, const QString& rcloneExecutable,
                        const QString& trigger, const QString& resticExecutable,
                        const PasswordOptions& passwordOptions) {
    if (thread_) {
        return makeError(ErrorCode::AlreadyExists, "a job is already running");
    }

    thread_ = std::make_unique<Thread>(databasePath_, job, std::move(provider), options,
                                       rcloneExecutable, trigger, resticExecutable,
                                       passwordOptions);
    runningJobId_ = job.id;

    connect(thread_.get(), &Thread::progressChanged, this, &JobWorker::progressChanged,
            Qt::QueuedConnection);
    connect(
        thread_.get(), &Thread::completed, this,
        [this](const RunReport& report) {
            if (thread_) {
                thread_->wait();
                thread_.reset();
            }
            runningJobId_ = 0;
            emit completed(report);
        },
        Qt::QueuedConnection);

    thread_->start();
    return Status();
}

void JobWorker::cancel() {
    if (thread_) {
        thread_->requestCancel();
    }
}

void JobWorker::waitForFinished(int timeoutMs) {
    if (!thread_) {
        return;
    }
    thread_->requestCancel();
    if (!thread_->wait(timeoutMs)) {
        logWarn("core.worker", QStringLiteral("worker thread did not stop within the timeout"));
    }
    thread_.reset();
    runningJobId_ = 0;
}

} // namespace nesva::jobs

#include "job_worker.moc"
