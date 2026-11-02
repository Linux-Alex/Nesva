// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/jobs/backup_runner.hpp"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include <utility>

#include "nesva/backup/restic.hpp"
#include "nesva/config/settings_store.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/db/database.hpp"
#include "nesva/security/secret_store.hpp"

namespace nesva::jobs {
namespace {

constexpr const char* kSnapshotTag = "nesva";

/// Collects restic-produced files from a staging tree and merges them into the
/// destination, honouring the conflict policy. Files are moved, not copied.
/// Longest common directory of the restored tree, so the snapshot's original
/// absolute layout is stripped and files land directly in the chosen folder.
QString commonRootOf(const QString& stagingRoot) {
    QStringList directories;
    QDirIterator iterator(stagingRoot, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        directories.append(QFileInfo(iterator.next()).absolutePath());
    }
    if (directories.isEmpty()) {
        return stagingRoot;
    }
    QString common = directories.constFirst();
    for (const QString& directory : directories) {
        while (!common.isEmpty() && directory != common &&
               !directory.startsWith(common + QLatin1Char('/'))) {
            common = QFileInfo(common).absolutePath();
        }
        if (common.isEmpty() || common == stagingRoot) {
            return stagingRoot;
        }
    }
    return common.isEmpty() ? stagingRoot : common;
}

int mergeStagedTree(const QString& stagingRoot, const QString& destinationRoot,
                    ConflictPolicy policy, int* skipped, int* renamed, qint64* bytes,
                    QStringList* errors) {
    int restored = 0;
    QDir root(commonRootOf(stagingRoot));
    QDirIterator iterator(stagingRoot, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString staged = iterator.next();
        const QString relative = root.relativeFilePath(staged);
        QString target = QDir(destinationRoot).absoluteFilePath(relative);

        if (QFileInfo::exists(target)) {
            switch (policy) {
            case ConflictPolicy::Skip:
                ++(*skipped);
                continue;
            case ConflictPolicy::Rename: {
                const QFileInfo info(target);
                QString candidate;
                for (int index = 1; index < 10000; ++index) {
                    const QString suffix = info.suffix().isEmpty()
                                               ? QString()
                                               : QStringLiteral(".") + info.suffix();
                    candidate = info.absolutePath() + QLatin1Char('/') + info.completeBaseName() +
                                QStringLiteral(" (%1)").arg(index) + suffix;
                    if (!QFileInfo::exists(candidate)) {
                        break;
                    }
                }
                if (candidate.isEmpty()) {
                    errors->append(QStringLiteral("%1: cannot find a free file name").arg(relative));
                    continue;
                }
                target = candidate;
                ++(*renamed);
                break;
            }
            case ConflictPolicy::Overwrite:
            case ConflictPolicy::Ask:
                QFile::remove(target);
                break;
            }
        }

        QDir().mkpath(QFileInfo(target).absolutePath());
        if (!QFile::rename(staged, target)) {
            // Cross-device or a permission problem: fall back to a copy so the
            // restore still completes, then remove the staged file.
            if (QFile::copy(staged, target)) {
                QFile::remove(staged);
            } else {
                errors->append(QStringLiteral("%1: cannot move the restored file into place")
                                   .arg(relative));
                continue;
            }
        }
        *bytes += QFileInfo(target).size();
        ++restored;
    }
    return restored;
}

} // namespace

// ---------------------------------------------------------------------------
// Password handling
// ---------------------------------------------------------------------------

QString repositoryAccount(const QString& repository) {
    const QByteArray digest =
        QCryptographicHash::hash(repository.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    return QStringLiteral("restic-") + QString::fromLatin1(digest);
}

Result<QByteArray> loadRepositoryPassword(const QString& repository,
                                          const PasswordOptions& options) {
    if (options.source == PasswordSource::File) {
        if (options.filePath.trimmed().isEmpty()) {
            return makeErrorValue(ErrorCode::ConfigurationError,
                                  "no repository password file is configured");
        }
        QFile file(options.filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            return makeErrorValue(ErrorCode::NotFound, "cannot read the repository password file",
                                  file.errorString().toStdString());
        }
        const QByteArray password = file.readAll().trimmed();
        if (password.isEmpty()) {
            return makeErrorValue(ErrorCode::ConfigurationError,
                                  "the repository password file is empty",
                                  options.filePath.toStdString());
        }
        return password;
    }

    std::unique_ptr<security::SecretStore> secrets = security::createSecretStore();
    if (!secrets->isAvailable()) {
        return makeErrorValue(ErrorCode::Unsupported,
                              "no credential store is available for the repository password",
                              secrets->unavailableReason().toStdString());
    }
    Result<QByteArray> secret =
        secrets->lookup(options.keyringService, repositoryAccount(repository));
    if (!secret) {
        return makeErrorValue(ErrorCode::NotFound, "no repository password is stored",
                              QStringLiteral("%1/%2: %3")
                                  .arg(options.keyringService, repositoryAccount(repository),
                                       QString::fromStdString(secret.error().message()))
                                  .toStdString());
    }
    return secret.value();
}

Status storeRepositoryPassword(const QString& repository, const PasswordOptions& options,
                               const QByteArray& password) {
    if (password.isEmpty()) {
        return makeError(ErrorCode::InvalidArgument, "the repository password must not be empty");
    }
    if (options.source == PasswordSource::File) {
        if (options.filePath.trimmed().isEmpty()) {
            return makeError(ErrorCode::ConfigurationError, "no password file is configured");
        }
        QFile file(options.filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return makeError(ErrorCode::IoError, "cannot write the repository password file",
                             file.errorString().toStdString());
        }
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        file.write(password);
        file.close();
        return Status();
    }

    std::unique_ptr<security::SecretStore> secrets = security::createSecretStore();
    if (!secrets->isAvailable()) {
        return makeError(ErrorCode::Unsupported,
                         "no credential store is available for the repository password",
                         secrets->unavailableReason().toStdString());
    }
    return secrets->store(options.keyringService, repositoryAccount(repository), password);
}

// ---------------------------------------------------------------------------
// BackupRunner
// ---------------------------------------------------------------------------

BackupRunner::BackupRunner(db::Database& database, RunProgressCallback progress,
                           std::shared_ptr<backup::ResticRunner> resticRunner)
    : database_(database),
      store_(database),
      progress_(std::move(progress)),
      resticRunner_(std::move(resticRunner)) {}

RunReport BackupRunner::run(const ArchiveJob& job, const RunOptions& options,
                            const CancellationToken& cancellation, const QString& trigger,
                            const QString& resticExecutable,
                            const PasswordOptions& passwordOptions) {
    RunReport report;
    report.jobId = job.id;
    report.jobName = job.name;
    report.dryRun = options.dryRun;

    RunProgress progress;
    progress.phase = RunPhase::Preparing;
    progress.message = QStringLiteral("Preparing the Restic repository");
    if (progress_) {
        progress_(progress);
    }

    // Verify the external tool before touching the repository, so a missing or
    // outdated restic produces one clear error instead of a partially recorded
    // run with a cryptic backend failure.
    const Status resticReady = backup::requireResticVersion(
        resticExecutable.isEmpty() ? QStringLiteral("restic") : resticExecutable);
    if (!resticReady.ok()) {
        report.status = QStringLiteral("failed");
        report.errors.append(QString::fromStdString(resticReady.error().toDisplayString()));
        return report;
    }

    if (!options.dryRun) {
        Result<qint64> runId = store_.beginRun(job.id, trigger);
        if (!runId) {
            report.status = QStringLiteral("failed");
            report.errors.append(QString::fromStdString(runId.error().toDisplayString()));
            return report;
        }
        report.runId = runId.value();
    }

    const QString repositoryLocation = job.destinationPath;
    if (repositoryLocation.trimmed().isEmpty()) {
        report.status = QStringLiteral("failed");
        const Error error(ErrorCode::ConfigurationError, "the job has no repository location");
        report.errors.append(QString::fromStdString(error.toDisplayString()));
        if (report.runId > 0) {
            store_.finishRun(report.runId, report.status, report.counters, error);
        }
        return report;
    }

    const Result<QByteArray> password =
        loadRepositoryPassword(repositoryLocation, passwordOptions);
    if (!password) {
        report.status = QStringLiteral("failed");
        report.errors.append(QString::fromStdString(password.error().toDisplayString()));
        if (report.runId > 0) {
            store_.finishRun(report.runId, report.status, report.counters, password.error());
            store_.appendJobEvent(job.id, report.runId, LogLevel::Error, QStringLiteral("backup"),
                                  QStringLiteral("Repository password unavailable"));
        }
        return report;
    }

    const Result<QString> passwordFile = backup::createPasswordFile(password.value());
    if (!passwordFile) {
        report.status = QStringLiteral("failed");
        report.errors.append(QString::fromStdString(passwordFile.error().toDisplayString()));
        if (report.runId > 0) {
            store_.finishRun(report.runId, report.status, report.counters, passwordFile.error());
        }
        return report;
    }
    // The temporary password file is removed on every exit path below.
    const auto removePasswordFile = [&passwordFile] { QFile::remove(passwordFile.value()); };

    backup::ResticRepository repository(resticExecutable.isEmpty() ? QStringLiteral("restic")
                                                                   : resticExecutable,
                                        repositoryLocation, passwordFile.value(), resticRunner_);

    QStringList sources;
    sources.reserve(job.sources.size());
    for (const JobSource& source : job.sources) {
        sources.append(QDir::cleanPath(source.path));
    }

    if (options.dryRun) {
        // A dry run for Restic mode checks connectivity and reports the plan
        // without writing anything to the repository.
        const Result<QVector<backup::SnapshotInfo>> snapshots = repository.snapshots();
        removePasswordFile();
        if (!snapshots) {
            report.status = QStringLiteral("failed");
            report.errors.append(QString::fromStdString(snapshots.error().toDisplayString()));
            return report;
        }
        report.status = QStringLiteral("succeeded");
        report.skipped = static_cast<int>(snapshots.value().size());
        report.counters.filesTotal = static_cast<qint64>(sources.size());
        return report;
    }

    progress.phase = RunPhase::Transferring;
    progress.message = QStringLiteral("Backing up with Restic");
    if (progress_) {
        progress_(progress);
    }

    const Result<backup::BackupSummary> summary =
        repository.backup(sources, job.exclusions, {QString::fromLatin1(kSnapshotTag)},
                          job.bandwidthLimitKiB, cancellation,
                          [&](const backup::BackupProgress& fileProgress) {
                              progress.filesTotal = fileProgress.totalFiles;
                              progress.bytesTotal = fileProgress.totalBytes;
                              progress.filesProcessed = fileProgress.filesDone;
                              progress.bytesTransferred = fileProgress.bytesDone;
                              progress.currentFile = fileProgress.currentFile;
                              progress.message = QStringLiteral("Backing up (%1%)")
                                                     .arg(static_cast<int>(fileProgress.percentDone * 100));
                              if (progress_) {
                                  progress_(progress);
                              }
                          });
    removePasswordFile();

    if (!summary) {
        const Error& error = summary.error();
        report.status = error.code() == ErrorCode::Cancelled ? QStringLiteral("cancelled")
                                                             : QStringLiteral("failed");
        report.errors.append(QString::fromStdString(error.toDisplayString()));
        if (report.runId > 0) {
            store_.finishRun(report.runId, report.status, report.counters, error);
            store_.appendJobEvent(job.id, report.runId, LogLevel::Error, QStringLiteral("backup"),
                                  QStringLiteral("Restic backup failed: %1")
                                      .arg(QString::fromStdString(error.message())));
        }
        return report;
    }

    const backup::BackupSummary info = summary.value();
    report.status = QStringLiteral("succeeded");
    report.counters.filesTotal = info.totalFilesProcessed;
    report.counters.filesTransferred = info.filesNew + info.filesChanged;
    report.counters.filesSkipped = info.filesUnmodified;
    report.counters.bytesTotal = info.totalBytesProcessed;
    report.counters.bytesTransferred = info.dataAdded;
    report.counters.filesProcessed = info.totalFilesProcessed;

    JobStore::SnapshotRecord snapshot;
    snapshot.snapshotId = info.snapshotId;
    snapshot.repositoryPath = repositoryLocation;
    snapshot.createdAt = info.finishedUtc.isValid()
                             ? info.finishedUtc.toString(Qt::ISODateWithMs)
                             : db::Database::currentTimestampUtc();
    snapshot.sizeBytes = info.totalBytesProcessed;
    snapshot.filesProcessed = static_cast<int>(info.totalFilesProcessed);
    snapshot.hostname = QSysInfo::machineHostName();
    store_.recordSnapshot(job.id, report.runId, snapshot);

    if (job.checkAfterBackup) {
        progress.phase = RunPhase::Finalising;
        progress.message = QStringLiteral("Verifying the repository");
        if (progress_) {
            progress_(progress);
        }
        const Result<QByteArray> verityPassword =
            loadRepositoryPassword(repositoryLocation, passwordOptions);
        if (verityPassword) {
            const Result<QString> checkPasswordFile =
                backup::createPasswordFile(verityPassword.value());
            if (checkPasswordFile) {
                backup::ResticRepository checkRepository(
                    resticExecutable.isEmpty() ? QStringLiteral("restic") : resticExecutable,
                    repositoryLocation, checkPasswordFile.value(), resticRunner_);
                const Result<backup::VerifyResult> check = checkRepository.check(QStringLiteral("5%"));
                QFile::remove(checkPasswordFile.value());
                if (!check || !check.value().ok) {
                    report.status = QStringLiteral("failed");
                    report.errors.append(QStringLiteral("post-backup verification failed"));
                }
            }
        }
    }

    std::optional<Error> runError;
    if (!report.succeeded()) {
        runError = Error(ErrorCode::IntegrityError, "post-backup verification reported problems");
    }
    store_.finishRun(report.runId, report.status, report.counters, runError);
    store_.appendJobEvent(
        job.id, report.runId, LogLevel::Info, QStringLiteral("backup"),
        QStringLiteral("Restic backup %1: snapshot %2, %3 new, %4 changed, %5 unchanged")
            .arg(report.status, info.snapshotId.left(8))
            .arg(info.filesNew)
            .arg(info.filesChanged)
            .arg(info.filesUnmodified),
        QJsonObject{{QStringLiteral("snapshot"), info.snapshotId},
                    {QStringLiteral("repository"), repositoryLocation}});

    progress.phase = RunPhase::Done;
    progress.message = QStringLiteral("Backup %1 (snapshot %2)")
                           .arg(report.status, info.snapshotId.left(8));
    if (progress_) {
        progress_(progress);
    }
    return report;
}

// ---------------------------------------------------------------------------
// BackupRestorer
// ---------------------------------------------------------------------------

BackupRestorer::BackupRestorer(db::Database& database) : database_(database) {}

BackupRestorer::Report BackupRestorer::restore(const Request& request,
                                               const CancellationToken& cancellation) {
    Report report;
    if (request.policy == ConflictPolicy::Ask) {
        report.errors.append(QStringLiteral(
            "the restore has unresolved conflicts: choose skip, overwrite or rename first"));
        return report;
    }

    const Result<QByteArray> password =
        loadRepositoryPassword(request.job.destinationPath, request.passwordOptions);
    if (!password) {
        report.errors.append(QString::fromStdString(password.error().toDisplayString()));
        return report;
    }
    const Result<QString> passwordFile = backup::createPasswordFile(password.value());
    if (!passwordFile) {
        report.errors.append(QString::fromStdString(passwordFile.error().toDisplayString()));
        return report;
    }

    QDir destination(request.destinationDirectory);
    if (!destination.exists() && !destination.mkpath(QStringLiteral("."))) {
        QFile::remove(passwordFile.value());
        report.errors.append(QStringLiteral("cannot create the restore directory"));
        return report;
    }

    // Restore into a staging directory next to the destination, then merge with
    // the conflict policy. restic itself always overwrites, which is exactly
    // what Nesva must not do silently.
    QTemporaryDir staging(QDir(request.destinationDirectory)
                              .absoluteFilePath(QStringLiteral(".nesva-restore-XXXXXX")));
    if (!staging.isValid()) {
        QFile::remove(passwordFile.value());
        report.errors.append(QStringLiteral("cannot create a staging directory for the restore"));
        return report;
    }

    backup::ResticRepository repository(
        request.resticExecutable.isEmpty() ? QStringLiteral("restic") : request.resticExecutable,
        request.job.destinationPath, passwordFile.value());
    const Result<QStringList> restored =
        repository.restore(request.snapshotId, staging.path(), {}, cancellation);
    QFile::remove(passwordFile.value());

    if (!restored) {
        report.status = restored.error().code() == ErrorCode::Cancelled
                            ? QStringLiteral("cancelled")
                            : QStringLiteral("failed");
        report.errors.append(QString::fromStdString(restored.error().toDisplayString()));
        return report;
    }

    int skipped = 0;
    int renamed = 0;
    qint64 bytes = 0;
    QStringList mergeErrors;
    const int merged = mergeStagedTree(staging.path(), request.destinationDirectory,
                                       request.policy, &skipped, &renamed, &bytes, &mergeErrors);
    report.restored = merged;
    report.skipped = skipped;
    report.renamed = renamed;
    report.bytesRestored = bytes;
    report.errors = mergeErrors;
    report.failed = static_cast<int>(mergeErrors.size());

    if (cancellation.isCancelled()) {
        report.status = QStringLiteral("cancelled");
    } else if (report.failed > 0) {
        report.status = QStringLiteral("failed");
    } else {
        report.status = QStringLiteral("succeeded");
    }

    JobStore store(database_);
    store.appendJobEvent(request.job.id, std::nullopt,
                         report.succeeded() ? LogLevel::Info : LogLevel::Error,
                         QStringLiteral("restore"),
                         QStringLiteral("Snapshot restore %1: %2 files, %3 skipped, %4 renamed")
                             .arg(report.status)
                             .arg(report.restored)
                             .arg(report.skipped)
                             .arg(report.renamed));
    return report;
}

} // namespace nesva::jobs
