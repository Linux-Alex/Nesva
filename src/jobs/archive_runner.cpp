// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/jobs/archive_runner.hpp"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>

#include <algorithm>
#include <utility>

#include "nesva/core/logging.hpp"
#include "nesva/core/paths.hpp"
#include "nesva/db/database.hpp"
#include "nesva/providers/provider_factory.hpp"
#include "nesva/scan/file_scanner.hpp"

namespace nesva::jobs {
namespace {

constexpr int kMaxRecordedErrors = 50;
constexpr int kCounterPersistInterval = 20;

QString timestampOf(const QDateTime& dateTime) {
    return dateTime.toUTC().toString(Qt::ISODateWithMs);
}

QString labelFor(const JobSource& source) {
    if (!source.label.trimmed().isEmpty()) {
        return source.label;
    }
    const QString name = QFileInfo(QDir::cleanPath(source.path)).fileName();
    const QString fallback = name.isEmpty() ? QStringLiteral("source") : name;
    return providers::sanitiseForRemote(fallback);
}

void appendError(RunReport& report, const QString& message) {
    ++report.failures;
    if (report.errors.size() < kMaxRecordedErrors) {
        report.errors.append(message);
    }
}

QString describeTransferError(const Error& error) {
    return QString::fromStdString(error.toDisplayString());
}

} // namespace

// ---------------------------------------------------------------------------
// Destination resolution
// ---------------------------------------------------------------------------

Result<ResolvedDestination> resolveDestination(const ArchiveJob& job,
                                               const std::optional<StorageProviderRecord>& provider,
                                               const QString& rcloneExecutable) {
    ResolvedDestination resolved;

    if (provider.has_value()) {
        if (provider->status == QStringLiteral("auth_required")) {
            return makeErrorValue(ErrorCode::ConfigurationError,
                                  "storage provider requires authentication",
                                  provider->name.toStdString());
        }
        if (provider->isRcloneBacked()) {
            if (provider->rcloneRemote.trimmed().isEmpty()) {
                return makeErrorValue(ErrorCode::ConfigurationError,
                                      "storage provider has no rclone remote configured",
                                      provider->name.toStdString());
            }
            resolved.selection.displayName = provider->name;
            resolved.selection.rcloneRemote = provider->rcloneRemote;
            resolved.selection.basePath =
                providers::joinRelative(provider->basePath, job.destinationPath);
        } else {
            resolved.selection.displayName = provider->name;
            resolved.selection.localRoot =
                QDir::cleanPath(QDir(provider->basePath).absoluteFilePath(job.destinationPath));
            resolved.localExclusionRoot = resolved.selection.localRoot;
        }
    } else {
        if (job.destinationPath.trimmed().isEmpty()) {
            return makeErrorValue(ErrorCode::ConfigurationError, "job has no destination configured",
                                  job.name.toStdString());
        }
        resolved.selection.displayName = QStringLiteral("Local folder");
        resolved.selection.localRoot =
            QDir::cleanPath(QFileInfo(job.destinationPath).absoluteFilePath());
        resolved.localExclusionRoot = resolved.selection.localRoot;
    }

    Result<std::unique_ptr<providers::StorageProvider>> created =
        providers::createProvider(resolved.selection, rcloneExecutable);
    if (!created) {
        return created.error();
    }
    resolved.provider = std::move(created).take();
    return resolved;
}

// ---------------------------------------------------------------------------
// ArchiveRunner
// ---------------------------------------------------------------------------

ArchiveRunner::ArchiveRunner(db::Database& database, RunProgressCallback progress)
    : database_(database), store_(database), progress_(std::move(progress)) {}

void ArchiveRunner::emitProgress(const RunProgress& progress) const {
    if (progress_) {
        progress_(progress);
    }
}

RunReport ArchiveRunner::run(const ArchiveJob& job,
                             const std::optional<StorageProviderRecord>& providerRecord,
                             const RunOptions& options, const CancellationToken& cancellation,
                             const QString& trigger, const QString& rcloneExecutable) {
    RunReport report;
    report.jobId = job.id;
    report.jobName = job.name;
    report.dryRun = options.dryRun;

    Result<scan::ExclusionRules> rules = scan::ExclusionRules::compile(job.exclusions);
    if (!rules) {
        report.status = QStringLiteral("failed");
        appendError(report, QString::fromStdString(rules.error().toDisplayString()));
        return report;
    }

    Result<ResolvedDestination> resolved =
        resolveDestination(job, providerRecord, rcloneExecutable);
    if (!resolved) {
        report.status = QStringLiteral("failed");
        appendError(report, QString::fromStdString(resolved.error().toDisplayString()));
        if (!options.dryRun) {
            // Record the failure in history so it is visible in the UI and CLI.
            Result<qint64> runId = store_.beginRun(job.id, trigger);
            if (runId) {
                store_.finishRun(runId.value(), QStringLiteral("failed"), report.counters,
                                 resolved.error());
                report.runId = runId.value();
            }
        }
        return report;
    }
    ResolvedDestination destination = std::move(resolved).take();

    // ---- run bookkeeping --------------------------------------------------
    // The run row is created before scanning so that a cancelled or failed
    // scan still shows up in history and blocks duplicate execution.
    RunProgress progress;
    if (!options.dryRun) {
        Result<qint64> runId = store_.beginRun(job.id, trigger);
        if (!runId) {
            report.status = QStringLiteral("failed");
            appendError(report, QString::fromStdString(runId.error().toDisplayString()));
            return report;
        }
        report.runId = runId.value();
        store_.appendJobEvent(job.id, report.runId, LogLevel::Info, QStringLiteral("archive"),
                              QStringLiteral("Archive run started"),
                              QJsonObject{{QStringLiteral("destination"),
                                           destination.provider->location()}});

        const Status ensured = destination.provider->ensureContainer();
        if (!ensured) {
            report.status = QStringLiteral("failed");
            appendError(report, QString::fromStdString(ensured.error().toDisplayString()));
            store_.finishRun(report.runId, report.status, report.counters, ensured.error());
            store_.appendJobEvent(job.id, report.runId, LogLevel::Error, QStringLiteral("archive"),
                                  QStringLiteral("Destination is not usable: %1")
                                      .arg(QString::fromStdString(ensured.error().message())));
            return report;
        }
    }

    // ---- scan -------------------------------------------------------------
    scan::ScanOptions scanOptions;
    scanOptions.exclusions = rules.value();
    scanOptions.followSymlinks = false;
    scanOptions.includeHidden = true;
    scanOptions.excludedRoots.append(Paths::cacheDirectory());
    if (!destination.localExclusionRoot.isEmpty()) {
        scanOptions.excludedRoots.append(destination.localExclusionRoot);
    }

    QVector<scan::ScanSource> sources;
    sources.reserve(job.sources.size());
    for (const JobSource& source : job.sources) {
        sources.append({QDir::cleanPath(source.path), labelFor(source)});
    }

    progress.phase = RunPhase::Preparing;
    progress.message = QStringLiteral("Scanning sources");
    emitProgress(progress);

    const auto sourceLabels = [&job] {
        QStringList labels;
        labels.reserve(job.sources.size());
        for (const JobSource& source : job.sources) {
            labels.append(labelFor(source));
        }
        return labels;
    }();

    // Map from source root to its label, so each scanned file knows where it
    // belongs on the destination.
    QHash<QString, QString> labelsByRoot;
    for (int index = 0; index < job.sources.size() && index < sourceLabels.size(); ++index) {
        labelsByRoot.insert(QDir::cleanPath(QFileInfo(job.sources.at(index).path).absoluteFilePath()),
                            sourceLabels.at(index));
    }

    const Result<scan::ScanResult> scanned =
        scan::scanSources(sources, scanOptions, cancellation, [&](const scan::ScanResult& snapshot) {
            RunProgress update;
            update.phase = RunPhase::Scanning;
            update.filesTotal = snapshot.fileCount();
            update.bytesTotal = snapshot.totalBytes;
            update.message = QStringLiteral("Scanning: %1 files").arg(snapshot.fileCount());
            emitProgress(update);
        });

    if (!scanned) {
        report.status = QStringLiteral("failed");
        appendError(report, QString::fromStdString(scanned.error().toDisplayString()));
        if (!options.dryRun) {
            store_.finishRun(report.runId, report.status, report.counters, scanned.error());
            store_.appendJobEvent(job.id, report.runId, LogLevel::Error, QStringLiteral("archive"),
                                  QStringLiteral("Scan failed: %1")
                                      .arg(QString::fromStdString(scanned.error().message())));
        }
        return report;
    }

    const scan::ScanResult scanResult = scanned.value();
    report.skippedEntries = scanResult.skipped;
    report.counters.filesTotal = scanResult.fileCount();
    report.counters.bytesTotal = scanResult.totalBytes;

    if (scanResult.cancelled) {
        // A cancelled scan must never be reported as a successful run, even
        // when it produced no entries at all.
        report.status = QStringLiteral("cancelled");
        if (!options.dryRun) {
            store_.finishRun(report.runId, report.status, report.counters,
                             Error(ErrorCode::Cancelled, "run cancelled during scan"));
            store_.appendJobEvent(job.id, report.runId, LogLevel::Warn, QStringLiteral("archive"),
                                  QStringLiteral("Archive run cancelled during scanning"));
        }
        return report;
    }

    progress.phase = RunPhase::Transferring;
    progress.filesTotal = scanResult.fileCount();
    progress.bytesTotal = scanResult.totalBytes;
    progress.message = QStringLiteral("Preparing destination");
    emitProgress(progress);

    // ---- previous manifest ------------------------------------------------
    QMap<QString, ManifestSnapshot> previous;
    if (!options.forceFullTransfer) {
        Result<QMap<QString, ManifestSnapshot>> loaded = store_.lastSuccessfulManifest(job.id);
        if (loaded) {
            previous = loaded.value();
        } else if (!options.dryRun) {
            logWarn("archive.runner", QStringLiteral("cannot read the previous manifest; "
                                                     "falling back to a full transfer"),
                    QJsonObject{{QStringLiteral("error"),
                                 QString::fromStdString(loaded.error().toDisplayString())}});
        }
    }

    // ---- transfer ---------------------------------------------------------
    providers::TransferOptions transferOptions;
    transferOptions.bandwidthLimitKiB = job.bandwidthLimitKiB;
    transferOptions.maxAttempts = options.maxAttempts;
    transferOptions.retryBackoffMs = options.retryBackoffMs;
    transferOptions.verifyChecksum = job.verifyChecksum;
    transferOptions.category = QStringLiteral("archive.transfer");

    progress.message = QStringLiteral("Transferring files");
    emitProgress(progress);

    int sincePersist = 0;
    int consecutiveFailures = 0;
    bool cancelled = false;
    QString fatalDestinationError;

    for (const scan::ScanEntry& entry : scanResult.files) {
        if (cancellation.isCancelled()) {
            cancelled = true;
            break;
        }

        const QString label = labelsByRoot.value(QDir::cleanPath(entry.sourceRoot),
                                                 providers::sanitiseForRemote(
                                                     QFileInfo(entry.sourceRoot).fileName()));
        const QString remoteRelative = providers::joinRelative(label, entry.relativePath);
        const QString modified = timestampOf(entry.modifiedUtc);

        const auto previousIt = previous.constFind(remoteRelative);
        const bool unchanged = previousIt != previous.constEnd() &&
                               previousIt->sizeBytes == entry.sizeBytes &&
                               previousIt->modifiedUtc == modified;

        ManifestRecord record;
        record.relativePath = remoteRelative;
        record.sourcePath = entry.absolutePath;
        record.sizeBytes = entry.sizeBytes;
        record.modifiedUtc = modified;

        if (unchanged) {
            ++report.counters.filesSkipped;
            ++report.skipped;
            ++report.counters.filesProcessed;
            record.state = QStringLiteral("skipped");
            record.skipReason = QStringLiteral("unchanged since the last successful run");
            record.sha256 = previousIt->sha256;
            if (!options.dryRun) {
                store_.recordManifestEntry(job.id, report.runId, record);
            }
        } else if (options.dryRun) {
            ++report.uploads;
            ++report.counters.filesTransferred;
            ++report.counters.filesProcessed;
            ++report.counters.bytesTransferred;
            report.counters.bytesTransferred += entry.sizeBytes;
            record.state = QStringLiteral("pending");
        } else {
            progress.currentFile = remoteRelative;
            progress.message = QStringLiteral("Transferring %1").arg(remoteRelative);
            emitProgress(progress);

            const Result<providers::TransferResult> transferred = destination.provider->uploadFile(
                entry.absolutePath, remoteRelative, transferOptions, cancellation,
                [&progress](const providers::TransferProgress& fileProgress) {
                    progress.bytesTransferred = fileProgress.bytesDone;
                    progress.bytesTotal = std::max(progress.bytesTotal, fileProgress.bytesTotal);
                });

            if (!transferred) {
                if (transferred.error().code() == ErrorCode::Cancelled) {
                    cancelled = true;
                    break;
                }
                ++report.counters.filesFailed;
                ++report.counters.filesProcessed;
                appendError(report, QStringLiteral("%1: %2").arg(
                                        remoteRelative, describeTransferError(transferred.error())));
                record.state = QStringLiteral("failed");
                record.skipReason = describeTransferError(transferred.error());
                store_.recordManifestEntry(job.id, report.runId, record);
                if (report.errors.size() <= 10) {
                    store_.appendJobEvent(job.id, report.runId, LogLevel::Warn,
                                          QStringLiteral("archive"),
                                          QStringLiteral("Could not archive %1: %2")
                                              .arg(remoteRelative,
                                                   QString::fromStdString(
                                                       transferred.error().message())));
                }
                ++consecutiveFailures;
                // A destination that cannot be written will fail for every
                // remaining file: stop instead of producing thousands of errors.
                // Provider-level destination failures abort immediately; for
                // protocol-level failures three in a row is treated as fatal.
                if (transferred.error().code() == ErrorCode::DestinationUnavailable ||
                    consecutiveFailures >= 3) {
                    fatalDestinationError = describeTransferError(transferred.error());
                    break;
                }
            } else {
                consecutiveFailures = 0;
                ++report.counters.filesTransferred;
                ++report.counters.filesProcessed;
                ++report.uploads;
                report.counters.bytesTransferred += entry.sizeBytes;
                record.state = transferred.value().sha256.isEmpty() ? QStringLiteral("transferred")
                                                                    : QStringLiteral("verified");
                record.sha256 = transferred.value().sha256;
                store_.recordManifestEntry(job.id, report.runId, record);
                store_.recordArchiveObject(job.id, report.runId, remoteRelative, entry.sizeBytes,
                                           transferred.value().sha256,
                                           record.state == QStringLiteral("verified")
                                               ? QStringLiteral("present")
                                               : QStringLiteral("unverified"));
            }
        }

        if (options.dryRun) {
            progress.filesProcessed = report.counters.filesProcessed;
            progress.filesTransferred = report.counters.filesTransferred;
            progress.filesSkipped = report.counters.filesSkipped;
            progress.filesFailed = report.counters.filesFailed;
            progress.bytesTransferred = report.counters.bytesTransferred;
            progress.currentFile = remoteRelative;
            emitProgress(progress);
            continue;
        }

        progress.filesProcessed = report.counters.filesProcessed;
        progress.filesTransferred = report.counters.filesTransferred;
        progress.filesSkipped = report.counters.filesSkipped;
        progress.filesFailed = report.counters.filesFailed;
        progress.bytesTransferred = report.counters.bytesTransferred;
        progress.currentFile = remoteRelative;
        emitProgress(progress);

        if (++sincePersist >= kCounterPersistInterval) {
            sincePersist = 0;
            store_.updateRunCounters(report.runId, report.counters);
        }
    }

    if (cancelled) {
        report.status = QStringLiteral("cancelled");
    } else if (!fatalDestinationError.isEmpty() || report.counters.filesFailed > 0) {
        report.status = QStringLiteral("failed");
    } else {
        report.status = QStringLiteral("succeeded");
    }

    if (options.dryRun) {
        progress.phase = RunPhase::Done;
        progress.message = QStringLiteral("Dry run: %1 to transfer, %2 unchanged")
                               .arg(report.uploads)
                               .arg(report.skipped);
        emitProgress(progress);
        return report;
    }

    std::optional<Error> runError;
    if (report.status == QStringLiteral("cancelled")) {
        runError = Error(ErrorCode::Cancelled, "run cancelled by the user");
    } else if (!fatalDestinationError.isEmpty()) {
        runError = Error(ErrorCode::IoError, "destination became unusable", fatalDestinationError.toStdString());
    } else if (report.counters.filesFailed > 0) {
        runError = Error(ErrorCode::IoError,
                         QStringLiteral("%1 file(s) could not be archived")
                             .arg(report.counters.filesFailed)
                             .toStdString());
    }

    store_.finishRun(report.runId, report.status, report.counters, runError);
    store_.appendJobEvent(
        job.id, report.runId,
        report.status == QStringLiteral("succeeded") ? LogLevel::Info : LogLevel::Error,
        QStringLiteral("archive"),
        QStringLiteral("Archive run %1: %2 transferred, %3 unchanged, %4 failed")
            .arg(report.status)
            .arg(report.counters.filesTransferred)
            .arg(report.counters.filesSkipped)
            .arg(report.counters.filesFailed),
        QJsonObject{{QStringLiteral("bytes_transferred"), report.counters.bytesTransferred},
                    {QStringLiteral("destination"), destination.provider->location()}});

    progress.phase = RunPhase::Done;
    progress.message = QStringLiteral("%1: %2 transferred, %3 unchanged, %4 failed")
                           .arg(report.status)
                           .arg(report.counters.filesTransferred)
                           .arg(report.counters.filesSkipped)
                           .arg(report.counters.filesFailed);
    progress.filesProcessed = report.counters.filesProcessed;
    emitProgress(progress);

    return report;
}

} // namespace nesva::jobs
