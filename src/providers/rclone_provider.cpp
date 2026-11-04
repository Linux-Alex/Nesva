// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/providers/rclone_provider.hpp"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QThread>

#include <utility>

#include "nesva/core/external_tool.hpp"
#include "nesva/core/logging.hpp"

namespace nesva::providers {
namespace {

constexpr int kDefaultTimeoutMs = 120000;
/// A batched copy of a large directory can legitimately take hours.
constexpr int kBatchTimeoutMs = 12 * 60 * 60 * 1000;

QString describeFailure(const ProcessOutcome& outcome, const QString& operation) {
    if (!outcome.started) {
        return QStringLiteral("%1: rclone could not be started").arg(operation);
    }
    if (outcome.cancelled) {
        return QStringLiteral("%1: cancelled").arg(operation);
    }
    if (outcome.timedOut) {
        return QStringLiteral("%1: rclone timed out").arg(operation);
    }
    QString detail = QString::fromLocal8Bit(outcome.standardError).trimmed();
    if (detail.isEmpty()) {
        detail = QString::fromLocal8Bit(outcome.standardOutput).trimmed();
    }
    if (detail.size() > 400) {
        detail = detail.left(400) + QStringLiteral("...");
    }
    return QStringLiteral("%1 failed (exit %2): %3").arg(operation).arg(outcome.exitCode).arg(detail);
}

bool looksLikeMissingPath(const ProcessOutcome& outcome) {
    const QString text = QString::fromLocal8Bit(outcome.standardError).toLower();
    return text.contains(QStringLiteral("directory not found")) ||
           text.contains(QStringLiteral("not found")) ||
           text.contains(QStringLiteral("object not found")) ||
           text.contains(QStringLiteral("no such file"));
}

} // namespace

// ---------------------------------------------------------------------------
// ProcessRunner
// ---------------------------------------------------------------------------

ProcessOutcome ProcessRunner::run(const QString& program, const QStringList& arguments,
                                 const CancellationToken& cancellation, int timeoutMs) {
    ProcessOutcome outcome;

    const QString resolved = resolveExecutable(program);
    if (resolved.isEmpty()) {
        outcome.started = false;
        outcome.startError = QStringLiteral("rclone executable not found: %1").arg(program);
        return outcome;
    }

    QProcess process;
    process.setProgram(resolved);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();

    if (!process.waitForStarted(timeoutMs)) {
        outcome.started = false;
        outcome.startError = process.errorString();
        return outcome;
    }

    const int pollIntervalMs = 100;
    int waitedMs = 0;
    while (process.state() != QProcess::NotRunning) {
        if (cancellation.isCancelled()) {
            process.kill();
            process.waitForFinished(2000);
            outcome.cancelled = true;
            outcome.exitCode = -1;
            return outcome;
        }
        if (waitedMs >= timeoutMs) {
            process.kill();
            process.waitForFinished(2000);
            outcome.timedOut = true;
            outcome.exitCode = -1;
            return outcome;
        }
        process.waitForFinished(pollIntervalMs);
        waitedMs += pollIntervalMs;
    }

    outcome.standardOutput = process.readAllStandardOutput();
    outcome.standardError = process.readAllStandardError();
    outcome.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    if (process.exitStatus() != QProcess::NormalExit) {
        outcome.started = false;
        outcome.startError = QStringLiteral("rclone crashed");
    }
    return outcome;
}

// ---------------------------------------------------------------------------
// RcloneProvider
// ---------------------------------------------------------------------------

RcloneProvider::RcloneProvider(QString remoteSpec, QString basePath, QString executable,
                               std::shared_ptr<ExternalRunner> runner)
    : remoteSpec_(std::move(remoteSpec)),
      basePath_(QDir::fromNativeSeparators(std::move(basePath))),
      executable_(executable.trimmed().isEmpty() ? QStringLiteral("rclone") : std::move(executable)),
      runner_(std::move(runner)) {
    if (!remoteSpec_.endsWith(QLatin1Char(':'))) {
        remoteSpec_ += QLatin1Char(':');
    }
    while (basePath_.endsWith(QLatin1Char('/'))) {
        basePath_.chop(1);
    }
    while (basePath_.startsWith(QLatin1Char('/'))) {
        basePath_.remove(0, 1);
    }
    if (!runner_) {
        runner_ = std::make_shared<ProcessRunner>();
    }
}

QString RcloneProvider::location() const {
    return basePath_.isEmpty() ? remoteSpec_ : remoteSpec_ + basePath_;
}

ProviderCapabilities RcloneProvider::capabilities() const {
    ProviderCapabilities capabilities;
    capabilities.canList = true;
    capabilities.canUpload = true;
    capabilities.canDownload = true;
    capabilities.canDelete = true;
    capabilities.canRename = true;
    // Hash verification depends on the backend; Nesva therefore verifies size
    // and only compares hashes when Rclone reports one.
    capabilities.canVerifyHash = false;
    capabilities.supportsAtomicMove = true;
    capabilities.supportsVersioning = false;
    capabilities.requiresInteractiveAuth = true;
    return capabilities;
}

Result<QString> RcloneProvider::remoteTargetFor(const QString& relativePath) const {
    const Result<QString> normalised = normaliseRelativePath(relativePath);
    if (!normalised) {
        return normalised.error();
    }
    const QString joined = joinRelative(basePath_, normalised.value());
    return remoteSpec_ + joined;
}

Result<ProcessOutcome> RcloneProvider::runRclone(const QStringList& arguments,
                                                const CancellationToken& cancellation,
                                                int timeoutMs) {
    logDebug("provider.rclone", QStringLiteral("running rclone"),
             QJsonObject{{QStringLiteral("arguments"), QJsonArray::fromStringList(arguments)}});
    ProcessOutcome outcome = runner_->run(executable_, arguments, cancellation, timeoutMs);
    if (outcome.exitCode != 0) {
        // Redaction happens in the logger; never echo rclone output to the UI
        // without going through it.
        logDebug("provider.rclone", QStringLiteral("rclone reported a failure"),
                 QJsonObject{{QStringLiteral("exit_code"), outcome.exitCode},
                             {QStringLiteral("stderr"),
                              QString::fromLocal8Bit(outcome.standardError).left(400)}});
    }
    return outcome;
}

Status RcloneProvider::testConnection() {
    const CancellationToken cancellation;
    // Listing the remote root validates the configuration and authentication
    // without touching the destination directory.
    const Result<ProcessOutcome> outcome =
        runRclone({QStringLiteral("lsd"), remoteSpec_, QStringLiteral("--max-depth"),
                   QStringLiteral("1")},
                  cancellation, 60000);
    if (!outcome) {
        return outcome.error();
    }
    if (outcome.value().exitCode != 0) {
        return makeError(ErrorCode::ExternalToolFailed, "rclone remote is not reachable",
                         describeFailure(outcome.value(), QStringLiteral("rclone lsd"))
                             .toStdString());
    }
    return Status();
}

ProcessOutcome ProcessRunner::runStreaming(const QString& program, const QStringList& arguments,
                                          const CancellationToken& cancellation, int timeoutMs,
                                          const std::function<void(const QByteArray&)>& onOutput) {
    ProcessOutcome outcome;
    const QString resolved = resolveExecutable(program);
    if (resolved.isEmpty()) {
        outcome.started = false;
        outcome.startError = QStringLiteral("executable not found: %1").arg(program);
        outcome.exitCode = -1;
        return outcome;
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(resolved, arguments);
    if (!process.waitForStarted(10000)) {
        outcome.started = false;
        outcome.startError = process.errorString();
        outcome.exitCode = -1;
        return outcome;
    }

    QElapsedTimer clock;
    clock.start();
    QByteArray buffer;
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(200);
        const QByteArray chunk = process.readAll();
        if (!chunk.isEmpty()) {
            buffer += chunk;
            if (onOutput) {
                onOutput(chunk);
            }
        }
        if (cancellation.isCancelled()) {
            outcome.cancelled = true;
            process.kill();
            process.waitForFinished(3000);
            break;
        }
        if (clock.elapsed() > timeoutMs) {
            outcome.timedOut = true;
            process.kill();
            process.waitForFinished(3000);
            break;
        }
    }
    process.waitForFinished(5000);
    buffer += process.readAll();
    outcome.standardOutput = buffer;
    outcome.standardError = buffer;
    outcome.exitCode = outcome.cancelled || outcome.timedOut
                           ? -1
                           : (process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1);
    return outcome;
}

Status RcloneProvider::uploadTree(const QString& localRoot, const QString& remoteRelativeBase,
                                  const TransferOptions& options,
                                  const CancellationToken& cancellation,
                                  const ProgressCallback& progress) {
    const Result<QString> target = remoteTargetFor(remoteRelativeBase);
    if (!target) {
        return target.error();
    }

    QStringList arguments = {QStringLiteral("copy"), localRoot, target.value(),
                             QStringLiteral("--transfers"),
                             QString::number(options.batchTransfers > 0 ? options.batchTransfers : 4),
                             QStringLiteral("--checkers"),
                             QString::number(options.batchTransfers > 0 ? options.batchTransfers : 4),
                             QStringLiteral("--stats"), QStringLiteral("1s"),
                             QStringLiteral("--use-json-log"),
                             QStringLiteral("--stats-log-level"), QStringLiteral("NOTICE")};
    if (options.bandwidthLimitKiB > 0) {
        arguments << QStringLiteral("--bwlimit")
                  << QStringLiteral("%1k").arg(options.bandwidthLimitKiB);
    }

    // rclone prints one JSON object per stats tick; parse the "stats" block so
    // the caller gets bytes/speed/ETA while the transfer is still running.
    QByteArray pending;
    const auto consume = [&pending, &progress](const QByteArray& chunk) {
        pending += chunk;
        int newline = pending.indexOf('\n');
        while (newline >= 0) {
            const QByteArray line = pending.left(newline);
            pending.remove(0, newline + 1);
            const QJsonDocument document = QJsonDocument::fromJson(line);
            if (!document.isObject()) {
                newline = pending.indexOf('\n');
                continue;
            }
            const QJsonObject stats = document.object().value(QStringLiteral("stats")).toObject();
            if (!stats.isEmpty() && progress) {
                TransferProgress update;
                update.bytesDone = static_cast<qint64>(stats.value(QStringLiteral("bytes")).toDouble(0));
                update.bytesTotal =
                    static_cast<qint64>(stats.value(QStringLiteral("totalBytes")).toDouble(0));
                update.bytesPerSecond =
                    static_cast<qint64>(stats.value(QStringLiteral("speed")).toDouble(0));
                const QJsonValue eta = stats.value(QStringLiteral("eta"));
                update.etaSeconds = eta.isDouble() ? static_cast<qint64>(eta.toDouble()) : -1;
                const QJsonArray transferring = stats.value(QStringLiteral("transferring")).toArray();
                if (!transferring.isEmpty()) {
                    update.currentFile =
                        transferring.first().toObject().value(QStringLiteral("name")).toString();
                }
                progress(update);
            }
            newline = pending.indexOf('\n');
        }
    };

    const ProcessOutcome outcome =
        runner_->runStreaming(executable_, arguments, cancellation, kBatchTimeoutMs, consume);
    if (outcome.cancelled) {
        return makeError(ErrorCode::Cancelled, "batch upload cancelled");
    }
    if (!outcome.started) {
        return makeError(ErrorCode::ExternalToolFailed, "cannot start rclone", outcome.startError.toStdString());
    }
    if (outcome.timedOut) {
        return makeError(ErrorCode::Timeout, "batch upload timed out");
    }
    if (outcome.exitCode != 0) {
        return makeError(ErrorCode::ExternalToolFailed, "rclone copy failed",
                         describeFailure(outcome, QStringLiteral("rclone copy")).toStdString());
    }
    return Status();
}

Status RcloneProvider::checkTree(const QString& localRoot, const QString& remoteRelativeBase,
                                 bool sizeOnly, const CancellationToken& cancellation) {
    const Result<QString> target = remoteTargetFor(remoteRelativeBase);
    if (!target) {
        return target.error();
    }
    QStringList arguments = {QStringLiteral("check"), localRoot, target.value(),
                             QStringLiteral("--one-way"), QStringLiteral("--combined"),
                             QStringLiteral("-")};
    if (sizeOnly) {
        arguments << QStringLiteral("--size-only");
    }
    const Result<ProcessOutcome> checked = runRclone(arguments, cancellation, kBatchTimeoutMs);
    if (!checked) {
        return checked.error();
    }
    const ProcessOutcome& outcome = checked.value();
    if (!outcome.started) {
        return makeError(ErrorCode::ExternalToolFailed, "cannot start rclone",
                         outcome.startError.toStdString());
    }
    if (outcome.exitCode == 0) {
        return Status();
    }
    // Exit code 1 means differences were found; list the first few.
    QStringList differences;
    const QStringList lines = QString::fromUtf8(outcome.standardOutput).split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        if (line.startsWith(QLatin1Char('*')) || line.startsWith(QLatin1Char('-'))) {
            differences.append(line.trimmed());
            if (differences.size() >= 5) {
                break;
            }
        }
    }
    return makeError(ErrorCode::IntegrityError, "the uploaded files do not match the source",
                     (differences.isEmpty() ? QStringLiteral("rclone check reported differences")
                                            : differences.join(QLatin1String("; ")))
                         .toStdString());
}

Result<qint64> RcloneProvider::freeSpaceBytes() {
    // "rclone about --json" reports total/used/free where the backend knows it.
    const CancellationToken cancellation;
    const Result<ProcessOutcome> outcome = runRclone(
        {QStringLiteral("about"), remoteSpec_, QStringLiteral("--json")}, cancellation, 60000);
    if (!outcome || outcome.value().exitCode != 0) {
        return qint64(-1); // unknown: never block a run on a missing number
    }
    const QJsonObject object =
        QJsonDocument::fromJson(outcome.value().standardOutput).object();
    if (object.contains(QStringLiteral("free"))) {
        return static_cast<qint64>(object.value(QStringLiteral("free")).toDouble(-1));
    }
    if (object.contains(QStringLiteral("total")) && object.contains(QStringLiteral("used"))) {
        return static_cast<qint64>(object.value(QStringLiteral("total")).toDouble(0) -
                                   object.value(QStringLiteral("used")).toDouble(0));
    }
    return qint64(-1);
}

Status RcloneProvider::ensureContainer() {
    const CancellationToken cancellation;
    const Result<ProcessOutcome> outcome = runRclone(
        {QStringLiteral("mkdir"), location()}, cancellation, 60000);
    if (!outcome) {
        return outcome.error();
    }
    if (outcome.value().exitCode != 0) {
        return makeError(ErrorCode::ExternalToolFailed, "cannot create remote destination",
                         describeFailure(outcome.value(), QStringLiteral("rclone mkdir"))
                             .toStdString());
    }
    return Status();
}

QStringList RcloneProvider::uploadArguments(const QString& localPath, const QString& remoteTarget,
                                            const TransferOptions& options) {
    QStringList arguments = {QStringLiteral("copyto"), localPath, remoteTarget,
                             QStringLiteral("--no-traverse"),
                             QStringLiteral("--stats"), QStringLiteral("0"),
                             QStringLiteral("--retries"), QStringLiteral("1"),
                             QStringLiteral("--low-level-retries"), QStringLiteral("2")};
    if (options.bandwidthLimitKiB > 0) {
        arguments << QStringLiteral("--bwlimit")
                  << QStringLiteral("%1k").arg(options.bandwidthLimitKiB);
    }
    if (options.verifyChecksum) {
        arguments << QStringLiteral("--checksum");
    }
    return arguments;
}

Result<TransferResult> RcloneProvider::uploadFile(const QString& localPath,
                                                  const QString& relativePath,
                                                  const TransferOptions& options,
                                          const CancellationToken& cancellation,
                                          const ProgressCallback& progress) {
    const Result<QString> target = remoteTargetFor(relativePath);
    if (!target) {
        return target.error();
    }

    const QFileInfo localInfo(localPath);
    if (!localInfo.exists() || !localInfo.isFile()) {
        return makeErrorValue(ErrorCode::NotFound, "source file disappeared before transfer",
                              localPath.toStdString());
    }

    const int attempts = options.maxAttempts > 0 ? options.maxAttempts : 1;
    int backoff = options.retryBackoffMs;

    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (cancellation.isCancelled()) {
            return makeErrorValue(ErrorCode::Cancelled, "transfer cancelled");
        }

        const Result<ProcessOutcome> outcome =
            runRclone(uploadArguments(localPath, target.value(), options), cancellation,
                      kDefaultTimeoutMs);
        if (!outcome) {
            return outcome.error();
        }
        const ProcessOutcome& result = outcome.value();

        if (result.cancelled) {
            return makeErrorValue(ErrorCode::Cancelled, "transfer cancelled");
        }
        if (result.exitCode == 0) {
            if (progress) {
                progress({localInfo.size(), localInfo.size()});
            }
            TransferResult transfer;
            transfer.bytesTransferred = localInfo.size();
            // Rclone does not report a hash for every backend; the manifest
            // therefore records the transfer as transferred, not verified.
            transfer.sha256.clear();
            return transfer;
        }

        const bool lastAttempt = attempt == attempts;
        if (lastAttempt) {
            return makeErrorValue(ErrorCode::ExternalToolFailed, "rclone upload failed",
                                  describeFailure(result, QStringLiteral("rclone copyto"))
                                      .toStdString());
        }
        logWarn("provider.rclone", QStringLiteral("upload failed; retrying"),
                QJsonObject{{QStringLiteral("attempt"), attempt},
                            {QStringLiteral("path"), relativePath},
                            {QStringLiteral("backoff_ms"), backoff}});
        QThread::msleep(static_cast<unsigned long>(backoff));
        backoff *= 2;
    }

    return makeErrorValue(ErrorCode::ExternalToolFailed, "rclone upload failed");
}

Result<TransferResult> RcloneProvider::downloadFile(const QString& relativePath,
                                                    const QString& localPath,
                                                    const TransferOptions& options,
                                            const CancellationToken& cancellation,
                                            const ProgressCallback& progress) {
    const Result<QString> target = remoteTargetFor(relativePath);
    if (!target) {
        return target.error();
    }

    QStringList arguments = {QStringLiteral("copyto"), target.value(), localPath,
                             QStringLiteral("--no-traverse"), QStringLiteral("--stats"),
                             QStringLiteral("0"), QStringLiteral("--retries"),
                             QStringLiteral("1")};
    if (options.bandwidthLimitKiB > 0) {
        arguments << QStringLiteral("--bwlimit")
                  << QStringLiteral("%1k").arg(options.bandwidthLimitKiB);
    }

    const Result<ProcessOutcome> outcome = runRclone(arguments, cancellation, kDefaultTimeoutMs);
    if (!outcome) {
        return outcome.error();
    }
    if (outcome.value().cancelled) {
        return makeErrorValue(ErrorCode::Cancelled, "restore cancelled");
    }
    if (outcome.value().exitCode != 0) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "rclone download failed",
                              describeFailure(outcome.value(), QStringLiteral("rclone copyto"))
                                  .toStdString());
    }

    const QFileInfo result(localPath);
    if (!result.exists()) {
        return makeErrorValue(ErrorCode::IntegrityError,
                              "rclone reported success but the file is missing",
                              localPath.toStdString());
    }
    if (progress) {
        progress({result.size(), result.size()});
    }
    TransferResult transfer;
    transfer.bytesTransferred = result.size();
    return transfer;
}

Result<bool> RcloneProvider::fileExists(const QString& relativePath) {
    const Result<QString> target = remoteTargetFor(relativePath);
    if (!target) {
        return target.error();
    }
    const CancellationToken cancellation;
    const Result<ProcessOutcome> outcome =
        runRclone({QStringLiteral("lsjson"), target.value(), QStringLiteral("--stat")},
                  cancellation, 60000);
    if (!outcome) {
        return outcome.error();
    }
    if (outcome.value().exitCode == 0) {
        return true;
    }
    if (looksLikeMissingPath(outcome.value())) {
        return false;
    }
    return makeErrorValue(ErrorCode::ExternalToolFailed, "cannot query remote object",
                          describeFailure(outcome.value(), QStringLiteral("rclone lsjson"))
                              .toStdString());
}

Result<QVector<RemoteEntry>> RcloneProvider::parseListing(const QByteArray& json) {
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "cannot parse rclone JSON output",
                              parseError.errorString().toStdString());
    }
    if (!document.isArray()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "unexpected rclone output (array expected)");
    }

    QVector<RemoteEntry> entries;
    const QJsonArray array = document.array();
    entries.reserve(array.size());
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        RemoteEntry entry;
        entry.relativePath = object.value(QStringLiteral("Path")).toString();
        entry.sizeBytes = static_cast<qint64>(object.value(QStringLiteral("Size")).toDouble());
        entry.isDirectory = object.value(QStringLiteral("IsDir")).toBool();
        const QString modified = object.value(QStringLiteral("ModTime")).toString();
        if (!modified.isEmpty()) {
            entry.modifiedUtc = QDateTime::fromString(modified, Qt::ISODateWithMs);
        }
        if (entry.relativePath.isEmpty()) {
            continue;
        }
        entries.append(entry);
    }
    return entries;
}

Result<QVector<RemoteEntry>> RcloneProvider::listFiles(const QString& directoryRelativePath,
                                                       bool recursive) {
    QString target = remoteSpec_ + basePath_;
    if (!directoryRelativePath.trimmed().isEmpty()) {
        const Result<QString> composed = remoteTargetFor(directoryRelativePath);
        if (!composed) {
            return composed.error();
        }
        target = composed.value();
    }

    QStringList arguments = {QStringLiteral("lsjson"), target};
    if (recursive) {
        arguments << QStringLiteral("--recursive");
    }

    const CancellationToken cancellation;
    const Result<ProcessOutcome> outcome = runRclone(arguments, cancellation, kDefaultTimeoutMs);
    if (!outcome) {
        return outcome.error();
    }
    if (outcome.value().exitCode != 0) {
        if (looksLikeMissingPath(outcome.value())) {
            return QVector<RemoteEntry>{};
        }
        return makeErrorValue(ErrorCode::ExternalToolFailed, "cannot list remote directory",
                              describeFailure(outcome.value(), QStringLiteral("rclone lsjson"))
                                  .toStdString());
    }
    return parseListing(outcome.value().standardOutput);
}

Result<QString> RcloneProvider::checksum(const QString& relativePath) {
    const Result<QString> target = remoteTargetFor(relativePath);
    if (!target) {
        return target.error();
    }

    const CancellationToken cancellation;
    const Result<ProcessOutcome> outcome =
        runRclone({QStringLiteral("hashsum"), QStringLiteral("SHA-256"), target.value()},
                  cancellation, 60000);
    if (!outcome) {
        return outcome.error();
    }
    if (outcome.value().exitCode != 0) {
        if (looksLikeMissingPath(outcome.value())) {
            return makeErrorValue(ErrorCode::NotFound, "archive object not found",
                                  relativePath.toStdString());
        }
        // Not every backend supports hashing; an empty value means "unverified",
        // which callers must not treat as success.
        logDebug("provider.rclone", QStringLiteral("remote cannot provide a checksum"),
                 QJsonObject{{QStringLiteral("path"), relativePath}});
        return QString();
    }

    const QString output = QString::fromLocal8Bit(outcome.value().standardOutput).trimmed();
    const QStringList parts = output.split(QRegularExpression(QStringLiteral("\\s+")),
                                           Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return QString();
    }
    return parts.constFirst().toLower();
}

} // namespace nesva::providers
