// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/jobs/archive_runner.hpp"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>

#include <algorithm>
#include <utility>

#include "nesva/core/disk_space.hpp"
#include "nesva/core/hashing.hpp"
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

    // Reachability and authentication are checked once per run. Without this a
    // stale iCloud session fails every single file with the same confusing
    // error; instead the provider is marked as needing re-authentication (the
    // dashboard and the Storage page surface that) and the run stops.
    if (!options.dryRun && providerRecord.has_value() && providerRecord->isRcloneBacked()) {
        const Status reachable = destination.provider->testConnection();
        if (!reachable.ok()) {
            const QString detail = QString::fromStdString(reachable.error().toDisplayString());
            const QString lowered = detail.toLower();
            const bool authIssue = lowered.contains(QStringLiteral("401")) ||
                                   lowered.contains(QStringLiteral("unauthoris")) ||
                                   lowered.contains(QStringLiteral("unauthoriz")) ||
                                   lowered.contains(QStringLiteral("2fa")) ||
                                   lowered.contains(QStringLiteral("two-factor")) ||
                                   lowered.contains(QStringLiteral("session")) ||
                                   lowered.contains(QStringLiteral("token")) ||
                                   lowered.contains(QStringLiteral("authentication")) ||
                                   lowered.contains(QStringLiteral("auth"));
            store_.updateProviderStatus(providerRecord->id,
                                        authIssue ? QStringLiteral("auth_required")
                                                  : QStringLiteral("error"),
                                        detail);
            report.status = QStringLiteral("failed");
            appendError(report,
                        authIssue
                            ? QStringLiteral("%1 needs to be signed in again: %2. Open Storage -> "
                                             "Configure / re-authenticate... and enter a fresh "
                                             "two-factor code.")
                                  .arg(providerRecord->name, detail)
                            : QStringLiteral("%1 is not reachable: %2")
                                  .arg(providerRecord->name, detail));
            return report;
        }
        store_.updateProviderStatus(providerRecord->id, QStringLiteral("ok"));
    }

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

    // ---- destination safety ------------------------------------------------
    // Writing into a destination that already holds files Nesva did not put
    // there can overwrite somebody's existing cloud data. Require an explicit
    // acknowledgement the first time.
    if (!options.dryRun && !job.allowExistingDestination) {
        const Result<std::optional<qint64>> previousRun = store_.latestRunId(job.id);
        const bool nesvaOwned = previousRun && previousRun.value().has_value();
        if (!nesvaOwned) {
            const Result<QVector<providers::RemoteEntry>> existing =
                destination.provider->listFiles(QString(), false);
            if (!existing) {
                report.status = QStringLiteral("failed");
                appendError(report,
                            QStringLiteral("cannot check the destination before writing: %1")
                                .arg(QString::fromStdString(existing.error().toDisplayString())));
                store_.finishRun(report.runId, report.status, report.counters, existing.error());
                return report;
            }
            if (!existing.value().isEmpty()) {
                const Error guarded(
                    ErrorCode::PermissionDenied,
                    "the destination already contains files that Nesva did not create",
                    QStringLiteral("%1 has %2 entries. Point the job at an empty or dedicated "
                                   "folder, or enable 'allow an existing destination' for this "
                                   "job if overwriting same-named files there is intended.")
                        .arg(destination.provider->location())
                        .arg(existing.value().size())
                        .toStdString());
                report.status = QStringLiteral("failed");
                appendError(report, QString::fromStdString(guarded.toDisplayString()));
                store_.finishRun(report.runId, report.status, report.counters, guarded);
                store_.appendJobEvent(job.id, report.runId, LogLevel::Error,
                                      QStringLiteral("archive"),
                                      QStringLiteral("Refused to write into a non-empty "
                                                     "destination that Nesva does not own"));
                return report;
            }
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

    // A local destination must be able to hold what is about to be written. The
    // message names another disk when one has room, instead of failing halfway.
    if (!options.dryRun && !destination.localExclusionRoot.isEmpty()) {
        const Status space = requireFreeSpace(destination.localExclusionRoot,
                                              scanResult.totalBytes,
                                              QStringLiteral("archiving this job"));
        if (!space.ok()) {
            report.status = QStringLiteral("failed");
            appendError(report, QString::fromStdString(space.error().toDisplayString()));
            store_.finishRun(report.runId, report.status, report.counters, space.error());
            return report;
        }
    }

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

                // "Upload succeeded" is not the same as "the bytes are there".
                // Ask the provider for the stored hash and compare it with the
                // file we sent; only a match is recorded as verified.
                QString remoteSha = transferred.value().sha256;
                if (job.verifyChecksum && remoteSha.isEmpty()) {
                    const Result<QString> remote = destination.provider->checksum(remoteRelative);
                    if (remote && !remote.value().isEmpty()) {
                        const Result<QString> local = sha256File(entry.absolutePath);
                        if (local && local.value().compare(remote.value(), Qt::CaseInsensitive) == 0) {
                            remoteSha = local.value();
                        } else if (local && !remote.value().isEmpty()) {
                            ++report.counters.filesFailed;
                            appendError(report,
                                        QStringLiteral("%1: the stored copy does not match the "
                                                       "file that was sent (expected %2, got %3)")
                                            .arg(remoteRelative, local.value(), remote.value()));
                            record.state = QStringLiteral("failed");
                            record.skipReason = QStringLiteral("checksum mismatch after upload");
                            store_.recordManifestEntry(job.id, report.runId, record);
                            continue;
                        }
                    }
                }
                record.state =
                    remoteSha.isEmpty() ? QStringLiteral("transferred") : QStringLiteral("verified");
                record.sha256 = remoteSha;
                store_.recordManifestEntry(job.id, report.runId, record);
                store_.recordArchiveObject(job.id, report.runId, remoteRelative, entry.sizeBytes,
                                           remoteSha,
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
    // Keep the database bounded: one manifest per run, forever, would grow on
    // the system disk with every backup.
    if (report.status == QStringLiteral("succeeded")) {
        store_.pruneManifestHistory(job.id, 5);
    }
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
