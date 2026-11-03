// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/jobs/mail_job_runner.hpp"

#include <QtCore/QJsonObject>

#include "nesva/core/logging.hpp"
#include "nesva/db/database.hpp"
#include "nesva/mail/exporter.hpp"
#include "nesva/mail/mail_store.hpp"
#include "nesva/mail/session.hpp"

namespace nesva::jobs {
namespace {

QString progressMessage(const mail::MailProgress& update) {
    QString message;
    if (update.percent() >= 0.0) {
        message = QStringLiteral("%1% of %2 messages")
                      .arg(QString::number(update.percent(), 'f', 1))
                      .arg(update.total);
    } else {
        message = QStringLiteral("%1 messages examined").arg(update.done);
    }
    if (update.messagesPerSecond > 0.0) {
        message += QStringLiteral(", %1 msg/s").arg(QString::number(update.messagesPerSecond,
                                                                    'f', 1));
    }
    if (update.etaSeconds >= 0) {
        message += QStringLiteral(", about %1 minute(s) left").arg(update.etaSeconds / 60);
    }
    return message;
}

} // namespace

MailJobRunner::MailJobRunner(db::Database& database, RunProgressCallback progress)
    : database_(database), progress_(std::move(progress)) {}

RunReport MailJobRunner::run(const ArchiveJob& job, const RunOptions& options,
                             const CancellationToken& cancellation, const QString& trigger) {
    JobStore store(database_);
    mail::MailStore mailStore(database_);

    RunReport report;
    report.jobId = job.id;
    report.jobName = job.name;
    report.dryRun = options.dryRun;

    if (job.mailAccountId <= 0) {
        report.status = QStringLiteral("failed");
        report.errors.append(QStringLiteral("the job has no Microsoft 365 account"));
        return report;
    }
    if (job.destinationPath.trimmed().isEmpty()) {
        report.status = QStringLiteral("failed");
        report.errors.append(
            QStringLiteral("the job has no destination directory for the .eml files"));
        return report;
    }

    const Result<mail::MailAccount> account = mailStore.account(job.mailAccountId);
    if (!account) {
        report.status = QStringLiteral("failed");
        report.errors.append(QString::fromStdString(account.error().toDisplayString()));
        return report;
    }

    if (options.dryRun) {
        const Result<QVector<mail::MailFolderStats>> stats = mailStore.folderStats(job.mailAccountId);
        if (!stats) {
            report.status = QStringLiteral("failed");
            report.errors.append(QString::fromStdString(stats.error().toDisplayString()));
            return report;
        }
        qint64 total = 0;
        qint64 archived = 0;
        for (const mail::MailFolderStats& folder : stats.value()) {
            if (!folder.syncEnabled) {
                continue;
            }
            total += folder.totalItems;
            archived += folder.exportedItems;
        }
        report.counters.filesTotal = total;
        report.counters.filesSkipped = archived;
        report.status = QStringLiteral("succeeded");
        return report;
    }

    const Result<qint64> runId = store.beginRun(job.id, trigger);
    if (!runId) {
        report.status = QStringLiteral("failed");
        report.errors.append(QString::fromStdString(runId.error().toDisplayString()));
        return report;
    }
    report.runId = runId.value();

    const Result<std::shared_ptr<mail::GraphClient>> graph =
        mail::openGraphClient(account.value());
    if (!graph) {
        report.status = QStringLiteral("failed");
        report.errors.append(QString::fromStdString(graph.error().toDisplayString()));
        store.finishRun(report.runId, report.status, report.counters, graph.error());
        store.appendJobEvent(job.id, report.runId, LogLevel::Error, QStringLiteral("mail"),
                             QStringLiteral("Sign-in required"));
        return report;
    }

    const auto onProgress = [this](const mail::MailProgress& update) {
        if (!progress_) {
            return;
        }
        RunProgress progress;
        progress.phase = RunPhase::Transferring;
        progress.currentFile = update.folder;
        progress.filesTotal = update.total;
        progress.filesProcessed = update.done;
        progress.filesTransferred = update.exported;
        progress.filesFailed = update.failed;
        progress.bytesTransferred = update.bytesWritten;
        progress.message = progressMessage(update);
        progress_(progress);
    };

    mail::MailExporter exporter(database_, graph.value(), onProgress);
    mail::MailExportOptions exportOptions;
    exportOptions.destinationDirectory = job.destinationPath;
    exportOptions.fullResync = options.forceFullTransfer;

    const Result<mail::MailExportReport> exported =
        exporter.run(job.mailAccountId, exportOptions, cancellation);
    if (!exported) {
        report.status = QStringLiteral("failed");
        report.errors.append(QString::fromStdString(exported.error().toDisplayString()));
        store.finishRun(report.runId, report.status, report.counters, exported.error());
        return report;
    }

    const mail::MailExportReport& outcome = exported.value();
    report.counters.filesTotal = outcome.totalMessages;
    report.counters.filesTransferred = outcome.exported;
    report.counters.filesSkipped = outcome.skipped;
    report.counters.filesFailed = outcome.failed;
    report.counters.bytesTransferred = outcome.bytesWritten;
    report.skipped = outcome.skipped;
    report.failures = outcome.failed;
    report.uploads = outcome.exported;
    report.errors = outcome.errors;

    if (cancellation.isCancelled()) {
        report.status = QStringLiteral("cancelled");
    } else if (outcome.failed > 0 || !outcome.errors.isEmpty()) {
        report.status = QStringLiteral("failed");
    } else {
        report.status = QStringLiteral("succeeded");
    }

    Error failure(ErrorCode::ExternalToolFailed, "mail archival reported errors");
    store.finishRun(report.runId, report.status, report.counters,
                    report.status == QStringLiteral("failed") ? failure : Error());
    logInfo("jobs.mail", QStringLiteral("mail job finished"),
            QJsonObject{{QStringLiteral("job"), job.name},
                        {QStringLiteral("exported"), outcome.exported},
                        {QStringLiteral("skipped"), outcome.skipped},
                        {QStringLiteral("failed"), outcome.failed},
                        {QStringLiteral("status"), report.status}});
    return report;
}

} // namespace nesva::jobs
