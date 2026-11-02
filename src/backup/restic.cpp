// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/backup/restic.hpp"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryFile>
#include <QtCore/QThread>

#include <algorithm>
#include <utility>

#include "nesva/core/external_tool.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/core/paths.hpp"

namespace nesva::backup {
namespace {

constexpr int kDefaultTimeoutMs = 30 * 60 * 1000;
constexpr int kShortTimeoutMs = 120 * 1000;
constexpr qint64 kMaxErrorTextLength = 600;

QString describeOutput(const QByteArray& bytes) {
    QString text = QString::fromLocal8Bit(bytes).trimmed();
    if (text.size() > kMaxErrorTextLength) {
        text = text.left(kMaxErrorTextLength) + QStringLiteral("...");
    }
    return text;
}

QJsonObject parseJsonObject(const QByteArray& line) {
    const QJsonDocument document = QJsonDocument::fromJson(line);
    return document.isObject() ? document.object() : QJsonObject();
}

QDateTime parseResticTime(const QString& value) {
    if (value.isEmpty()) {
        return QDateTime();
    }
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    return parsed.isValid() ? parsed.toUTC() : QDateTime();
}

} // namespace

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

Error mapResticFailure(const ResticOutcome& outcome, const QString& operation) {
    const QString detail = describeOutput(outcome.standardError.isEmpty() ? outcome.standardOutput
                                                                         : outcome.standardError);
    if (!outcome.started) {
        return Error(ErrorCode::ExternalToolFailed,
                     operation.toStdString() + ": restic could not be started", outcome.startError.toStdString());
    }
    if (outcome.cancelled) {
        return Error(ErrorCode::Cancelled, operation.toStdString() + ": cancelled");
    }
    if (outcome.timedOut) {
        return Error(ErrorCode::Timeout, operation.toStdString() + ": restic timed out");
    }

    switch (outcome.exitCode) {
    case 10:
        return Error(ErrorCode::NotFound, operation.toStdString() + ": repository does not exist",
                     detail.toStdString());
    case 11:
        return Error(ErrorCode::AlreadyExists,
                     operation.toStdString() + ": repository is locked by another restic process",
                     detail.toStdString());
    case 12:
        return Error(ErrorCode::PermissionDenied, operation.toStdString() + ": incorrect password",
                     detail.toStdString());
    default:
        break;
    }
    return Error(ErrorCode::ExternalToolFailed,
                 operation.toStdString() + ": restic exited with code " +
                     std::to_string(outcome.exitCode),
                 detail.toStdString());
}

// ---------------------------------------------------------------------------
// ProcessResticRunner
// ---------------------------------------------------------------------------

ResticOutcome ProcessResticRunner::run(const QStringList& arguments,
                                      const CancellationToken& cancellation, int timeoutMs,
                                      const std::function<void(const QByteArray&)>& onLine) {
    ResticOutcome outcome;
    const QString program = resolveExecutable(QStringLiteral("restic"));
    if (program.isEmpty()) {
        outcome.started = false;
        outcome.startError = QStringLiteral("restic executable not found on PATH");
        return outcome;
    }

    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(10000)) {
        outcome.started = false;
        outcome.startError = process.errorString();
        return outcome;
    }

    QByteArray pendingLine;
    // stdout is consumed incrementally for line callbacks, so it is also
    // accumulated here: parsers that need the whole document must not see an
    // already-drained buffer.
    QByteArray capturedOutput;
    const auto drain = [&](bool flushPartial) {
        const QByteArray chunk = process.readAllStandardOutput();
        capturedOutput += chunk;
        pendingLine += chunk;
        int newline = pendingLine.indexOf('\n');
        while (newline >= 0) {
            const QByteArray line = pendingLine.left(newline).trimmed();
            pendingLine.remove(0, newline + 1);
            if (!line.isEmpty() && onLine) {
                onLine(line);
            }
            newline = pendingLine.indexOf('\n');
        }
        if (flushPartial && !pendingLine.trimmed().isEmpty()) {
            if (onLine) {
                onLine(pendingLine.trimmed());
            }
            pendingLine.clear();
        }
    };

    int waitedMs = 0;
    const int pollMs = 100;
    while (process.state() != QProcess::NotRunning) {
        if (cancellation.isCancelled()) {
            process.kill();
            process.waitForFinished(3000);
            outcome.cancelled = true;
            return outcome;
        }
        if (waitedMs >= timeoutMs) {
            process.kill();
            process.waitForFinished(3000);
            outcome.timedOut = true;
            return outcome;
        }
        process.waitForFinished(pollMs);
        waitedMs += pollMs;
        drain(false);
    }
    drain(true);

    outcome.standardOutput = capturedOutput + process.readAllStandardOutput();
    outcome.standardError = process.readAllStandardError();
    outcome.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    if (process.exitStatus() != QProcess::NormalExit) {
        outcome.started = false;
        outcome.startError = QStringLiteral("restic crashed");
    }
    return outcome;
}

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

namespace command {

QStringList base(const QString& repository, const QString& passwordFile) {
    QStringList arguments = {QStringLiteral("--repo"), repository};
    if (!passwordFile.isEmpty()) {
        arguments << QStringLiteral("--password-file") << passwordFile;
    }
    return arguments;
}

QStringList init(const QString& repository, const QString& passwordFile) {
    QStringList arguments = base(repository, passwordFile);
    arguments << QStringLiteral("init");
    return arguments;
}

QStringList backup(const QString& repository, const QString& passwordFile,
                   const QStringList& sources, const QVector<scan::ExclusionRule>& exclusions,
                   const QStringList& tags, qint64 bandwidthLimitKiB) {
    QStringList arguments = base(repository, passwordFile);
    arguments << QStringLiteral("backup");
    arguments << sources;
    arguments << QStringLiteral("--json");
    for (const scan::ExclusionRule& rule : exclusions) {
        // Restic globs are matched against the whole path; a pattern without a
        // slash is applied to the file name, which is what the UI promises.
        const QString pattern = rule.pattern.trimmed();
        if (pattern.isEmpty()) {
            continue;
        }
        if (rule.kind == scan::PatternKind::Regex) {
            arguments << QStringLiteral("--iexclude") << pattern;
        } else {
            arguments << QStringLiteral("--exclude") << pattern;
        }
    }
    for (const QString& tag : tags) {
        arguments << QStringLiteral("--tag") << tag;
    }
    if (bandwidthLimitKiB > 0) {
        arguments << QStringLiteral("--limit-upload") << QString::number(bandwidthLimitKiB);
    }
    return arguments;
}

QStringList snapshots(const QString& repository, const QString& passwordFile,
                      const QStringList& tags) {
    QStringList arguments = base(repository, passwordFile);
    arguments << QStringLiteral("snapshots") << QStringLiteral("--json");
    for (const QString& tag : tags) {
        arguments << QStringLiteral("--tag") << tag;
    }
    return arguments;
}

QStringList listSnapshot(const QString& repository, const QString& passwordFile,
                         const QString& snapshotId) {
    QStringList arguments = base(repository, passwordFile);
    arguments << QStringLiteral("ls") << snapshotId << QStringLiteral("--json");
    return arguments;
}

QStringList restore(const QString& repository, const QString& passwordFile,
                    const QString& snapshotId, const QString& target,
                    const QStringList& includePaths) {
    QStringList arguments = base(repository, passwordFile);
    arguments << QStringLiteral("restore") << snapshotId << QStringLiteral("--target") << target;
    for (const QString& include : includePaths) {
        arguments << QStringLiteral("--include") << include;
    }
    return arguments;
}

QStringList forget(const QString& repository, const QString& passwordFile,
                   const RetentionPolicy& policy, bool dryRun, bool prune) {
    QStringList arguments = base(repository, passwordFile);
    arguments << QStringLiteral("forget") << QStringLiteral("--json");
    if (policy.keepLast > 0) {
        arguments << QStringLiteral("--keep-last") << QString::number(policy.keepLast);
    }
    if (policy.keepDaily > 0) {
        arguments << QStringLiteral("--keep-daily") << QString::number(policy.keepDaily);
    }
    if (policy.keepWeekly > 0) {
        arguments << QStringLiteral("--keep-weekly") << QString::number(policy.keepWeekly);
    }
    if (policy.keepMonthly > 0) {
        arguments << QStringLiteral("--keep-monthly") << QString::number(policy.keepMonthly);
    }
    if (policy.keepYearly > 0) {
        arguments << QStringLiteral("--keep-yearly") << QString::number(policy.keepYearly);
    }
    if (dryRun) {
        arguments << QStringLiteral("--dry-run");
    }
    if (prune) {
        arguments << QStringLiteral("--prune");
    }
    return arguments;
}

QStringList check(const QString& repository, const QString& passwordFile,
                  const QString& readDataSubset) {
    QStringList arguments = base(repository, passwordFile);
    arguments << QStringLiteral("check") << QStringLiteral("--json");
    if (!readDataSubset.trimmed().isEmpty()) {
        arguments << QStringLiteral("--read-data-subset") << readDataSubset.trimmed();
    }
    return arguments;
}

QStringList listLocks(const QString& repository, const QString& passwordFile) {
    QStringList arguments = base(repository, passwordFile);
    arguments << QStringLiteral("list") << QStringLiteral("locks");
    return arguments;
}

QStringList unlock(const QString& repository, const QString& passwordFile) {
    QStringList arguments = base(repository, passwordFile);
    arguments << QStringLiteral("unlock");
    return arguments;
}

} // namespace command

// ---------------------------------------------------------------------------
// Retention policy
// ---------------------------------------------------------------------------

QJsonObject retentionToJson(const RetentionPolicy& policy) {
    QJsonObject object;
    object.insert(QStringLiteral("keep_last"), policy.keepLast);
    object.insert(QStringLiteral("keep_daily"), policy.keepDaily);
    object.insert(QStringLiteral("keep_weekly"), policy.keepWeekly);
    object.insert(QStringLiteral("keep_monthly"), policy.keepMonthly);
    object.insert(QStringLiteral("keep_yearly"), policy.keepYearly);
    return object;
}

RetentionPolicy retentionFromJson(const QJsonObject& object) {
    RetentionPolicy policy;
    policy.keepLast = object.value(QStringLiteral("keep_last")).toInt(policy.keepLast);
    policy.keepDaily = object.value(QStringLiteral("keep_daily")).toInt(policy.keepDaily);
    policy.keepWeekly = object.value(QStringLiteral("keep_weekly")).toInt(policy.keepWeekly);
    policy.keepMonthly = object.value(QStringLiteral("keep_monthly")).toInt(policy.keepMonthly);
    policy.keepYearly = object.value(QStringLiteral("keep_yearly")).toInt(policy.keepYearly);
    return policy;
}

// ---------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------

Result<QVector<SnapshotInfo>> ResticRepository::parseSnapshots(const QByteArray& json) {
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "cannot parse restic snapshots output",
                              parseError.errorString().toStdString());
    }
    if (!document.isArray()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "unexpected restic snapshots output");
    }

    QVector<SnapshotInfo> snapshots;
    for (const QJsonValue& value : document.array()) {
        const QJsonObject object = value.toObject();
        SnapshotInfo info;
        info.id = object.value(QStringLiteral("id")).toString();
        if (info.id.isEmpty()) {
            info.id = object.value(QStringLiteral("short_id")).toString();
        }
        info.shortId = object.value(QStringLiteral("short_id")).toString();
        if (info.shortId.isEmpty() && info.id.size() >= 8) {
            info.shortId = info.id.left(8);
        }
        info.time = parseResticTime(object.value(QStringLiteral("time")).toString());
        info.hostname = object.value(QStringLiteral("hostname")).toString();
        for (const QJsonValue& path : object.value(QStringLiteral("paths")).toArray()) {
            info.paths.append(path.toString());
        }
        for (const QJsonValue& tag : object.value(QStringLiteral("tags")).toArray()) {
            info.tags.append(tag.toString());
        }
        const QJsonObject summary = object.value(QStringLiteral("summary")).toObject();
        if (!summary.isEmpty()) {
            info.totalBytes =
                static_cast<qint64>(summary.value(QStringLiteral("total_bytes_processed")).toDouble(-1));
            info.filesProcessed = summary.value(QStringLiteral("total_files_processed")).toInt(-1);
        }
        if (info.id.isEmpty()) {
            continue;
        }
        snapshots.append(info);
    }
    return snapshots;
}

Result<BackupProgress> ResticRepository::parseBackupStatus(const QJsonObject& object) {
    BackupProgress progress;
    progress.percentDone = object.value(QStringLiteral("percent_done")).toDouble(0.0);
    progress.filesDone = static_cast<qint64>(object.value(QStringLiteral("files_done")).toDouble(0));
    progress.totalFiles = static_cast<qint64>(object.value(QStringLiteral("total_files")).toDouble(0));
    progress.bytesDone = static_cast<qint64>(object.value(QStringLiteral("bytes_done")).toDouble(0));
    progress.totalBytes = static_cast<qint64>(object.value(QStringLiteral("total_bytes")).toDouble(0));
    const QJsonArray current = object.value(QStringLiteral("current_files")).toArray();
    if (!current.isEmpty()) {
        progress.currentFile = current.at(0).toString();
    }
    return progress;
}

Result<BackupSummary> ResticRepository::parseBackupLines(const QVector<QByteArray>& lines,
                                                         BackupProgress* lastProgress) {
    BackupSummary summary;
    bool foundSummary = false;

    for (const QByteArray& line : lines) {
        const QJsonObject object = parseJsonObject(line);
        if (object.isEmpty()) {
            continue;
        }
        const QString type = object.value(QStringLiteral("message_type")).toString();
        if (type == QStringLiteral("status")) {
            if (lastProgress != nullptr) {
                *lastProgress = parseBackupStatus(object).valueOr(BackupProgress{});
            }
        } else if (type == QStringLiteral("error")) {
            const QString message = object.value(QStringLiteral("message")).toString();
            return makeErrorValue(ErrorCode::ExternalToolFailed, "restic reported an error",
                                  message.toStdString());
        } else if (type == QStringLiteral("summary")) {
            foundSummary = true;
            summary.snapshotId = object.value(QStringLiteral("snapshot_id")).toString();
            summary.filesNew =
                static_cast<qint64>(object.value(QStringLiteral("files_new")).toDouble(0));
            summary.filesChanged =
                static_cast<qint64>(object.value(QStringLiteral("files_changed")).toDouble(0));
            summary.filesUnmodified =
                static_cast<qint64>(object.value(QStringLiteral("files_unmodified")).toDouble(0));
            summary.totalFilesProcessed =
                static_cast<qint64>(object.value(QStringLiteral("total_files_processed")).toDouble(0));
            summary.totalBytesProcessed =
                static_cast<qint64>(object.value(QStringLiteral("total_bytes_processed")).toDouble(0));
            summary.dataAdded =
                static_cast<qint64>(object.value(QStringLiteral("data_added")).toDouble(0));
            summary.startedUtc =
                parseResticTime(object.value(QStringLiteral("backup_start")).toString());
            summary.finishedUtc =
                parseResticTime(object.value(QStringLiteral("backup_end")).toString());
        }
    }

    if (!foundSummary) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "restic finished without a backup summary");
    }
    return summary;
}

Result<QVector<FileEntry>> ResticRepository::parseListLines(const QVector<QByteArray>& lines) {
    QVector<FileEntry> entries;
    for (const QByteArray& line : lines) {
        const QJsonObject object = parseJsonObject(line);
        if (object.isEmpty()) {
            continue;
        }
        const QString messageType = object.value(QStringLiteral("message_type")).toString();
        if (messageType != QStringLiteral("node")) {
            continue;
        }
        FileEntry entry;
        entry.path = object.value(QStringLiteral("path")).toString();
        entry.name = object.value(QStringLiteral("name")).toString();
        entry.type = object.value(QStringLiteral("type")).toString();
        entry.sizeBytes = static_cast<qint64>(object.value(QStringLiteral("size")).toDouble(0));
        entry.modifiedUtc = parseResticTime(object.value(QStringLiteral("mtime")).toString());
        if (entry.path.isEmpty()) {
            continue;
        }
        entries.append(entry);
    }
    return entries;
}

Result<QVector<ForgetGroup>> ResticRepository::parseForgetOutput(const QByteArray& json) {
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "cannot parse restic forget output",
                              parseError.errorString().toStdString());
    }
    if (!document.isArray()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "unexpected restic forget output");
    }

    QVector<ForgetGroup> groups;
    for (const QJsonValue& value : document.array()) {
        const QJsonObject object = value.toObject();
        ForgetGroup group;
        for (const QJsonValue& snapshot : object.value(QStringLiteral("keep")).toArray()) {
            const QString id = snapshot.toObject().value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) {
                group.keptIds.append(id);
            }
        }
        // "remove" is null when the policy removes nothing.
        for (const QJsonValue& snapshot : object.value(QStringLiteral("remove")).toArray()) {
            const QString id = snapshot.toObject().value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) {
                group.removedIds.append(id);
            }
        }
        groups.append(group);
    }
    return groups;
}

Result<VerifyResult> ResticRepository::parseCheckLines(const QVector<QByteArray>& lines) {
    VerifyResult result;
    for (const QByteArray& line : lines) {
        const QJsonObject object = parseJsonObject(line);
        if (object.isEmpty()) {
            continue;
        }
        const QString type = object.value(QStringLiteral("message_type")).toString();
        if (type == QStringLiteral("error")) {
            ++result.errors;
            result.messages.append(object.value(QStringLiteral("message")).toString());
        } else if (type == QStringLiteral("summary")) {
            result.errors =
                static_cast<qint64>(object.value(QStringLiteral("num_errors")).toDouble(result.errors));
        } else if (type == QStringLiteral("status")) {
            // Progress only; ignore.
        }
    }
    result.ok = result.errors == 0;
    return result;
}

Result<QVector<LockInfo>> ResticRepository::parseLockList(const QByteArray& output) {
    QVector<LockInfo> locks;
    for (const QString& token : QString::fromLocal8Bit(output).split(QRegularExpression(
             QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
        LockInfo info;
        info.id = token;
        locks.append(info);
    }
    return locks;
}

// ---------------------------------------------------------------------------
// ResticRepository
// ---------------------------------------------------------------------------

ResticRepository::ResticRepository(QString executable, QString repository, QString passwordFile,
                                   std::shared_ptr<ResticRunner> runner)
    : executable_(executable.trimmed().isEmpty() ? QStringLiteral("restic") : std::move(executable)),
      repository_(std::move(repository)),
      passwordFile_(std::move(passwordFile)),
      runner_(std::move(runner)) {
    if (!runner_) {
        runner_ = std::make_shared<ProcessResticRunner>();
    }
    Q_UNUSED(executable_);
}

Result<QVector<QByteArray>> ResticRepository::runCollect(
    const QStringList& arguments, const CancellationToken& cancellation, int timeoutMs,
    ResticOutcome* outcome, const std::function<void(const QByteArray&)>& onLine) {
    QVector<QByteArray> lines;
    ResticOutcome collected;
    // The executable is resolved by the runner; the argument vector carries the
    // repository and password file, never the password itself.
    collected = runner_->run(arguments, cancellation, timeoutMs, [&](const QByteArray& line) {
        if (onLine) {
            onLine(line);
        }
        lines.append(line);
    });
    if (outcome != nullptr) {
        *outcome = collected;
    }
    if (collected.cancelled) {
        return makeErrorValue(ErrorCode::Cancelled, "restic operation cancelled");
    }
    return lines;
}

Status ResticRepository::init() {
    const CancellationToken cancellation;
    ResticOutcome outcome;
    const Result<QVector<QByteArray>> lines = runCollect(
        command::init(repository_, passwordFile_), cancellation, kShortTimeoutMs, &outcome);
    Q_UNUSED(lines);
    if (outcome.exitCode == 0) {
        return Status();
    }
    // An existing repository is not an error when the caller just wants a usable
    // repository, but init must say so explicitly.
    return Status(mapResticFailure(outcome, QStringLiteral("restic init")));
}

Result<BackupSummary> ResticRepository::backup(
    const QStringList& sources, const QVector<scan::ExclusionRule>& exclusions,
    const QStringList& tags, qint64 bandwidthLimitKiB, const CancellationToken& cancellation,
    const std::function<void(const BackupProgress&)>& progress) {
    if (sources.isEmpty()) {
        return makeErrorValue(ErrorCode::InvalidArgument, "no sources to back up");
    }

    QVector<QByteArray> collected;
    ResticOutcome outcome = runner_->run(
        command::backup(repository_, passwordFile_, sources, exclusions, tags, bandwidthLimitKiB),
        cancellation, kDefaultTimeoutMs, [&](const QByteArray& line) {
            collected.append(line);
            if (!progress || line.size() < 2) {
                return;
            }
            const QJsonObject object = parseJsonObject(line);
            if (object.value(QStringLiteral("message_type")).toString() ==
                QStringLiteral("status")) {
                progress(parseBackupStatus(object).valueOr(BackupProgress{}));
            }
        });

    if (outcome.cancelled) {
        return makeErrorValue(ErrorCode::Cancelled, "backup cancelled");
    }
    if (outcome.exitCode != 0) {
        return Error(mapResticFailure(outcome, QStringLiteral("restic backup")));
    }
    return parseBackupLines(collected);
}

Result<QVector<SnapshotInfo>> ResticRepository::snapshots(const QStringList& tags) {
    const CancellationToken cancellation;
    ResticOutcome outcome;
    const Result<QVector<QByteArray>> lines = runCollect(
        command::snapshots(repository_, passwordFile_, tags), cancellation, kShortTimeoutMs,
        &outcome);
    if (!lines) {
        return lines.error();
    }
    if (outcome.exitCode != 0) {
        return Error(mapResticFailure(outcome, QStringLiteral("restic snapshots")));
    }
    return parseSnapshots(outcome.standardOutput);
}

Result<QVector<FileEntry>> ResticRepository::listSnapshot(const QString& snapshotId) {
    const CancellationToken cancellation;
    ResticOutcome outcome;
    const Result<QVector<QByteArray>> lines = runCollect(
        command::listSnapshot(repository_, passwordFile_, snapshotId), cancellation,
        kDefaultTimeoutMs, &outcome);
    if (!lines) {
        return lines.error();
    }
    if (outcome.exitCode != 0) {
        return Error(mapResticFailure(outcome, QStringLiteral("restic ls")));
    }
    return parseListLines(lines.value());
}

Result<QStringList> ResticRepository::restore(const QString& snapshotId, const QString& target,
                                             const QStringList& includePaths,
                                             const CancellationToken& cancellation) {
    ResticOutcome outcome;
    QStringList messages;
    const Result<QVector<QByteArray>> lines = runCollect(
        command::restore(repository_, passwordFile_, snapshotId, target, includePaths),
        cancellation, kDefaultTimeoutMs, &outcome);
    if (!lines) {
        return lines.error();
    }
    for (const QByteArray& line : lines.value()) {
        messages.append(QString::fromLocal8Bit(line));
    }
    if (outcome.exitCode != 0) {
        return Error(mapResticFailure(outcome, QStringLiteral("restic restore")));
    }
    return messages;
}

Result<QVector<ForgetGroup>> ResticRepository::forget(const RetentionPolicy& policy, bool dryRun,
                                                     bool prune) {
    if (policy.isEmpty()) {
        return makeErrorValue(ErrorCode::InvalidArgument,
                              "the retention policy keeps nothing; refusing to delete everything");
    }
    const CancellationToken cancellation;
    ResticOutcome outcome;
    const Result<QVector<QByteArray>> lines = runCollect(
        command::forget(repository_, passwordFile_, policy, dryRun, prune), cancellation,
        kDefaultTimeoutMs, &outcome);
    if (!lines) {
        return lines.error();
    }
    if (outcome.exitCode != 0) {
        return Error(mapResticFailure(outcome, QStringLiteral("restic forget")));
    }
    return parseForgetOutput(outcome.standardOutput);
}

Result<VerifyResult> ResticRepository::check(const QString& readDataSubset) {
    const CancellationToken cancellation;
    ResticOutcome outcome;
    const Result<QVector<QByteArray>> lines = runCollect(
        command::check(repository_, passwordFile_, readDataSubset), cancellation, kDefaultTimeoutMs,
        &outcome);
    if (!lines) {
        return lines.error();
    }
    const Result<VerifyResult> parsed = parseCheckLines(lines.value());
    if (outcome.exitCode != 0 && parsed && parsed.value().errors == 0) {
        return Error(mapResticFailure(outcome, QStringLiteral("restic check")));
    }
    return parsed;
}

Result<QVector<LockInfo>> ResticRepository::listLocks() {
    const CancellationToken cancellation;
    ResticOutcome outcome;
    const Result<QVector<QByteArray>> lines = runCollect(
        command::listLocks(repository_, passwordFile_), cancellation, kShortTimeoutMs, &outcome);
    if (!lines) {
        return lines.error();
    }
    if (outcome.exitCode != 0) {
        return Error(mapResticFailure(outcome, QStringLiteral("restic list locks")));
    }
    return parseLockList(outcome.standardOutput);
}

Status ResticRepository::unlock(bool removeAll) {
    const CancellationToken cancellation;
    ResticOutcome outcome;
    QStringList arguments = command::unlock(repository_, passwordFile_);
    if (removeAll) {
        arguments << QStringLiteral("--remove-all");
    }
    const Result<QVector<QByteArray>> lines =
        runCollect(arguments, cancellation, kShortTimeoutMs, &outcome);
    Q_UNUSED(lines);
    if (outcome.exitCode != 0) {
        return Status(mapResticFailure(outcome, QStringLiteral("restic unlock")));
    }
    return Status();
}

Result<QString> createPasswordFile(const QByteArray& password) {
    const Status directories = Paths::ensureBaseDirectories();
    if (!directories) {
        return directories.error();
    }
    const QDir cache(Paths::cacheDirectory());
    QTemporaryFile file(cache.absoluteFilePath(QStringLiteral("restic-password-XXXXXX")));
    file.setAutoRemove(false);
    if (!file.open()) {
        return makeErrorValue(ErrorCode::IoError, "cannot create the restic password file",
                              file.errorString().toStdString());
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(password);
    file.flush();
    const QString path = file.fileName();
    file.close();
    return path;
}

} // namespace nesva::backup
