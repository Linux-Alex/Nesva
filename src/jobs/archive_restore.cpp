// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/jobs/archive_restore.hpp"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include <utility>

#include "nesva/core/hashing.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/db/database.hpp"
#include "nesva/jobs/archive_runner.hpp"
#include "nesva/providers/storage_provider.hpp"

namespace nesva::jobs {
namespace {

constexpr int kMaxSamples = 5;

/// Returns a non-existing path derived from \p target: "name (1).ext", "name (2).ext", ...
QString uniqueDestination(const QString& target) {
    const QFileInfo info(target);
    const QString directory = info.absolutePath();
    const QString completeBase = info.completeBaseName();
    const QString suffix = info.suffix();

    for (int index = 1; index < 10000; ++index) {
        const QString candidate =
            suffix.isEmpty()
                ? QStringLiteral("%1 (%2)").arg(completeBase).arg(index)
                : QStringLiteral("%1 (%2).%3").arg(completeBase).arg(index).arg(suffix);
        const QString path = QDir(directory).absoluteFilePath(candidate);
        if (!QFileInfo::exists(path)) {
            return path;
        }
    }
    return QString();
}

} // namespace

QString conflictPolicyToString(ConflictPolicy policy) {
    switch (policy) {
    case ConflictPolicy::Skip:
        return QStringLiteral("skip");
    case ConflictPolicy::Overwrite:
        return QStringLiteral("overwrite");
    case ConflictPolicy::Rename:
        return QStringLiteral("rename");
    case ConflictPolicy::Ask:
        break;
    }
    return QStringLiteral("ask");
}

ConflictPolicy conflictPolicyFromString(const QString& value, ConflictPolicy fallback) {
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("skip")) {
        return ConflictPolicy::Skip;
    }
    if (normalised == QStringLiteral("overwrite")) {
        return ConflictPolicy::Overwrite;
    }
    if (normalised == QStringLiteral("rename")) {
        return ConflictPolicy::Rename;
    }
    if (normalised == QStringLiteral("ask")) {
        return ConflictPolicy::Ask;
    }
    return fallback;
}

ArchiveRestorer::ArchiveRestorer(db::Database& database, RestoreProgressCallback progress)
    : database_(database), store_(database), progress_(std::move(progress)) {}

Result<RestorePlan> ArchiveRestorer::plan(const ArchiveJob& job,
                                          const std::optional<StorageProviderRecord>& provider,
                                          const QString& destinationDirectory,
                                          const QStringList& onlyPaths,
                                          const QString& rcloneExecutable,
                                          bool checkDestination) {
    Result<ResolvedDestination> resolved = resolveDestination(job, provider, rcloneExecutable);
    if (!resolved) {
        return resolved.error();
    }
    ResolvedDestination destination = std::move(resolved).take();

    const Result<QVector<providers::RemoteEntry>> listing =
        destination.provider->listFiles(QString(), true);
    if (!listing) {
        return listing.error();
    }

    // Manifest metadata (hashes, state) is joined by relative path; the
    // provider remains the source of truth for what actually exists remotely.
    QMap<QString, ManifestRecord> manifestByPath;
    Result<QVector<ManifestRecord>> manifest = store_.latestManifest(job.id);
    if (manifest) {
        for (const ManifestRecord& record : manifest.value()) {
            manifestByPath.insert(record.relativePath, record);
        }
    }

    RestorePlan plan;
    const QDir destinationDirectoryInfo(destinationDirectory);

    for (const providers::RemoteEntry& entry : listing.value()) {
        if (entry.isDirectory) {
            continue;
        }
        if (!onlyPaths.isEmpty() && !onlyPaths.contains(entry.relativePath)) {
            continue;
        }

        ArchiveItem item;
        item.relativePath = entry.relativePath;
        item.sizeBytes = entry.sizeBytes;
        item.modifiedUtc = entry.modifiedUtc;

        const auto manifestIt = manifestByPath.constFind(entry.relativePath);
        if (manifestIt != manifestByPath.constEnd()) {
            item.sha256 = manifestIt->sha256;
            item.state = manifestIt->state;
        }

        if (checkDestination) {
            const QString target = QDir::toNativeSeparators(
                destinationDirectoryInfo.absoluteFilePath(entry.relativePath));
            if (QFileInfo::exists(target)) {
                ++plan.conflictingFiles;
                if (plan.conflictSamples.size() < kMaxSamples) {
                    plan.conflictSamples.append(entry.relativePath);
                }
            }
        }

        plan.totalBytes += entry.sizeBytes;
        plan.items.append(item);
    }

    return plan;
}

RestoreReport ArchiveRestorer::restore(const RestoreRequest& request,
                                       const CancellationToken& cancellation,
                                       const QString& rcloneExecutable) {
    RestoreReport report;

    if (request.policy == ConflictPolicy::Ask) {
        report.status = QStringLiteral("failed");
        report.errors.append(QStringLiteral(
            "the restore has unresolved conflicts: choose skip, overwrite or rename first"));
        return report;
    }

    Result<ResolvedDestination> resolved =
        resolveDestination(request.job, request.providerRecord, rcloneExecutable);
    if (!resolved) {
        report.errors.append(QString::fromStdString(resolved.error().toDisplayString()));
        return report;
    }
    ResolvedDestination destination = std::move(resolved).take();

    const QDir destinationDirectory(request.destinationDirectory);
    if (!destinationDirectory.exists() && !destinationDirectory.mkpath(QStringLiteral("."))) {
        report.errors.append(QStringLiteral("cannot create restore directory: %1")
                                 .arg(request.destinationDirectory));
        return report;
    }

    Result<RestorePlan> planned =
        plan(request.job, request.providerRecord, request.destinationDirectory,
             request.onlyPaths, rcloneExecutable);
    if (!planned) {
        report.errors.append(QString::fromStdString(planned.error().toDisplayString()));
        return report;
    }
    const RestorePlan plan = planned.value();

    // Record the restore request before touching the filesystem.
    {
        QSqlQuery insert(database_.handle());
        insert.prepare(QStringLiteral(
            "INSERT INTO restore_jobs (job_id, source_kind, source_selector, destination_path, "
            "conflict_policy, status, started_at, created_at) "
            "VALUES (?, 'archive', ?, ?, ?, 'running', ?, ?)"));
        insert.addBindValue(request.job.id);
        insert.addBindValue(destination.provider->location());
        insert.addBindValue(QDir::cleanPath(request.destinationDirectory));
        insert.addBindValue(conflictPolicyToString(request.policy));
        insert.addBindValue(db::Database::currentTimestampUtc());
        insert.addBindValue(db::Database::currentTimestampUtc());
        if (!insert.exec()) {
            report.errors.append(QStringLiteral("cannot record the restore: %1")
                                     .arg(insert.lastError().text()));
            return report;
        }
        report.restoreJobId = insert.lastInsertId().toLongLong();
    }

    store_.appendJobEvent(request.job.id, std::nullopt, LogLevel::Info, QStringLiteral("restore"),
                          QStringLiteral("Restore started into %1")
                              .arg(QDir::cleanPath(request.destinationDirectory)),
                          QJsonObject{{QStringLiteral("files"), plan.items.size()},
                                      {QStringLiteral("policy"),
                                       conflictPolicyToString(request.policy)},
                                      {QStringLiteral("conflicts"), plan.conflictingFiles}});

    providers::TransferOptions transferOptions;
    transferOptions.maxAttempts = request.maxAttempts;
    transferOptions.retryBackoffMs = request.retryBackoffMs;
    transferOptions.verifyChecksum = request.verify;
    transferOptions.category = QStringLiteral("restore.transfer");

    RestoreProgress progress;
    progress.filesTotal = plan.items.size();
    if (progress_) {
        progress_(progress);
    }

    bool cancelled = false;

    for (const ArchiveItem& item : plan.items) {
        if (cancellation.isCancelled()) {
            cancelled = true;
            break;
        }

        const Result<QString> safeRelative = providers::normaliseRelativePath(item.relativePath);
        if (!safeRelative) {
            ++report.failed;
            report.errors.append(QStringLiteral("%1: invalid archive path").arg(item.relativePath));
            continue;
        }

        const QString baseTarget =
            QDir::toNativeSeparators(destinationDirectory.absoluteFilePath(safeRelative.value()));

        QString target = baseTarget;
        if (QFileInfo::exists(baseTarget)) {
            switch (request.policy) {
            case ConflictPolicy::Skip:
                ++report.skipped;
                ++progress.filesDone;
                if (progress_) {
                    progress_(progress);
                }
                continue;
            case ConflictPolicy::Rename:
                target = uniqueDestination(baseTarget);
                if (target.isEmpty()) {
                    ++report.failed;
                    report.errors.append(
                        QStringLiteral("%1: could not find a free file name").arg(item.relativePath));
                    continue;
                }
                ++report.renamed;
                break;
            case ConflictPolicy::Overwrite:
            case ConflictPolicy::Ask:
                break;
            }
        }

        progress.currentFile = safeRelative.value();
        progress.message = QStringLiteral("Restoring %1").arg(safeRelative.value());
        if (progress_) {
            progress_(progress);
        }

        const Result<providers::TransferResult> transferred = destination.provider->downloadFile(
            safeRelative.value(), target, transferOptions, cancellation,
            [&progress](const providers::TransferProgress& fileProgress) {
                progress.bytesDone = fileProgress.bytesDone;
            });

        if (!transferred) {
            if (transferred.error().code() == ErrorCode::Cancelled) {
                cancelled = true;
                break;
            }
            ++report.failed;
            report.errors.append(QStringLiteral("%1: %2").arg(
                safeRelative.value(),
                QString::fromStdString(transferred.error().toDisplayString())));
            continue;
        }

        // Post-restore verification against the recorded manifest hash. A
        // missing hash means "unverified", which is reported, not hidden.
        if (request.verify && !item.sha256.isEmpty()) {
            const Result<QString> restoredHash = sha256File(target);
            if (!restoredHash) {
                ++report.failed;
                report.errors.append(QStringLiteral("%1: %2").arg(
                    safeRelative.value(),
                    QString::fromStdString(restoredHash.error().toDisplayString())));
                QFile::remove(target);
                continue;
            }
            if (restoredHash.value() != item.sha256) {
                ++report.failed;
                report.errors.append(
                    QStringLiteral("%1: checksum mismatch after restore").arg(safeRelative.value()));
                QFile::remove(target);
                continue;
            }
            report.verifiedFiles.append(safeRelative.value());
        }

        ++report.restored;
        report.bytesRestored += transferred.value().bytesTransferred;
        ++progress.filesDone;
        if (progress_) {
            progress_(progress);
        }
    }

    if (cancelled) {
        report.status = QStringLiteral("cancelled");
    } else if (report.failed > 0) {
        report.status = QStringLiteral("failed");
    } else {
        report.status = QStringLiteral("succeeded");
    }

    {
        QSqlQuery update(database_.handle());
        update.prepare(QStringLiteral(
            "UPDATE restore_jobs SET status = ?, finished_at = ?, files_restored = ?, "
            "bytes_restored = ?, error_message = ? WHERE id = ?"));
        update.addBindValue(report.status);
        update.addBindValue(db::Database::currentTimestampUtc());
        update.addBindValue(report.restored);
        update.addBindValue(report.bytesRestored);
        update.addBindValue(report.errors.isEmpty() ? QVariant()
                                                    : QVariant(report.errors.join(QLatin1Char('\n'))));
        update.addBindValue(report.restoreJobId);
        if (!update.exec()) {
            logWarn("restore", QStringLiteral("cannot update the restore record"),
                    QJsonObject{{QStringLiteral("error"), update.lastError().text()}});
        }
    }

    store_.appendJobEvent(
        request.job.id, std::nullopt,
        report.status == QStringLiteral("succeeded") ? LogLevel::Info : LogLevel::Error,
        QStringLiteral("restore"),
        QStringLiteral("Restore %1: %2 restored, %3 skipped, %4 failed, %5 verified")
            .arg(report.status)
            .arg(report.restored)
            .arg(report.skipped)
            .arg(report.failed)
            .arg(report.verifiedFiles.size()));

    progress.message = QStringLiteral("%1: %2 restored, %3 skipped, %4 failed")
                           .arg(report.status)
                           .arg(report.restored)
                           .arg(report.skipped)
                           .arg(report.failed);
    if (progress_) {
        progress_(progress);
    }

    return report;
}

} // namespace nesva::jobs
