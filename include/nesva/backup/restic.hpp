// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <functional>
#include <memory>

#include "nesva/core/cancellation.hpp"
#include "nesva/core/result.hpp"
#include "nesva/scan/exclusions.hpp"

namespace nesva::backup {

/// Outcome of one restic invocation, with restic's documented exit codes
/// already mapped to Nesva error codes.
struct ResticOutcome {
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;
    bool started = true;
    bool timedOut = false;
    bool cancelled = false;
    QString startError;
};

/// Runs restic. Injected so argument construction, output parsing and error
/// mapping are testable without a real repository.
class ResticRunner {
public:
    virtual ~ResticRunner() = default;

    /// \param onLine called for every complete stdout line while the command
    ///        runs (restic prints newline-delimited JSON for backup/ls/check).
    virtual ResticOutcome run(const QStringList& arguments, const CancellationToken& cancellation,
                              int timeoutMs,
                              const std::function<void(const QByteArray&)>& onLine = {}) = 0;
};

/// Production runner: QProcess, separate channels, no shell, kill on cancel.
class ProcessResticRunner final : public ResticRunner {
public:
    ResticOutcome run(const QStringList& arguments, const CancellationToken& cancellation,
                      int timeoutMs,
                      const std::function<void(const QByteArray&)>& onLine = {}) override;
};

/// Restores the documented exit codes: 0 ok, 10 repository missing,
/// 11 already locked, 12 wrong password, everything else a generic failure.
Error mapResticFailure(const ResticOutcome& outcome, const QString& operation);

/// Retention policy as applied by "restic forget".
struct RetentionPolicy {
    int keepLast = 0;
    int keepDaily = 7;
    int keepWeekly = 4;
    int intUnused = 0;
    int keepMonthly = 6;
    int keepYearly = 2;

    bool isEmpty() const {
        return keepLast == 0 && keepDaily == 0 && keepWeekly == 0 && keepMonthly == 0 &&
               keepYearly == 0;
    }
};

QJsonObject retentionToJson(const RetentionPolicy& policy);
RetentionPolicy retentionFromJson(const QJsonObject& object);

struct SnapshotInfo {
    QString id;
    QString shortId;
    QDateTime time;
    QString hostname;
    QStringList paths;
    QStringList tags;
    qint64 totalBytes = -1; ///< -1 when the repository does not report it
    int filesProcessed = -1;
};

struct BackupProgress {
    double percentDone = 0.0;
    qint64 filesDone = 0;
    qint64 totalFiles = 0;
    qint64 bytesDone = 0;
    qint64 totalBytes = 0;
    QString currentFile;
};

struct BackupSummary {
    QString snapshotId;
    qint64 filesNew = 0;
    qint64 filesChanged = 0;
    qint64 filesUnmodified = 0;
    qint64 totalFilesProcessed = 0;
    qint64 totalBytesProcessed = 0;
    qint64 dataAdded = 0;
    QDateTime startedUtc;
    QDateTime finishedUtc;
};

struct FileEntry {
    QString path;     ///< absolute path as stored in the snapshot
    QString name;
    QString type;     ///< "file" | "dir" | "symlink" | ...
    qint64 sizeBytes = 0;
    QDateTime modifiedUtc;
};

struct LockInfo {
    QString id;
};

struct ForgetGroup {
    QStringList keptIds;
    QStringList removedIds;
};

struct VerifyResult {
    qint64 errors = 0;
    bool ok = true;
    QStringList messages;
};

/// Argument builders. Pure functions so the exact argv can be asserted.
namespace command {
QStringList base(const QString& repository, const QString& passwordFile);
QStringList init(const QString& repository, const QString& passwordFile);
QStringList backup(const QString& repository, const QString& passwordFile,
                   const QStringList& sources, const QVector<scan::ExclusionRule>& exclusions,
                   const QStringList& tags, qint64 bandwidthLimitKiB);
QStringList snapshots(const QString& repository, const QString& passwordFile,
                      const QStringList& tags);
QStringList listSnapshot(const QString& repository, const QString& passwordFile,
                         const QString& snapshotId);
QStringList restore(const QString& repository, const QString& passwordFile,
                    const QString& snapshotId, const QString& target,
                    const QStringList& includePaths);
QStringList forget(const QString& repository, const QString& passwordFile,
                   const RetentionPolicy& policy, bool dryRun, bool prune);
QStringList check(const QString& repository, const QString& passwordFile,
                  const QString& readDataSubset);
QStringList listLocks(const QString& repository, const QString& passwordFile);
QStringList unlock(const QString& repository, const QString& passwordFile);
} // namespace command

/// A Restic repository at \p repository (a path, or an rclone/rest backend).
///
/// The password is supplied through a temporary file with 0600 permissions that
/// the caller owns and removes; restic never receives it on the command line
/// (where it would appear in the process list).
class ResticRepository {
public:
    ResticRepository(QString executable, QString repository, QString passwordFile,
                     std::shared_ptr<ResticRunner> runner = nullptr);

    const QString& location() const { return repository_; }

    /// Creates the repository. Fails with AlreadyExists when one is present.
    Status init();

    /// Runs a backup of \p sources. Progress is delivered per restic status line.
    Result<BackupSummary> backup(const QStringList& sources,
                                 const QVector<scan::ExclusionRule>& exclusions,
                                 const QStringList& tags, qint64 bandwidthLimitKiB,
                                 const CancellationToken& cancellation,
                                 const std::function<void(const BackupProgress&)>& progress = {});

    Result<QVector<SnapshotInfo>> snapshots(const QStringList& tags = {});

    Result<QVector<FileEntry>> listSnapshot(const QString& snapshotId);

    /// Restores a snapshot into \p target. restic writes files itself, so this
    /// never overwrites: the caller plans conflicts first (see jobs::BackupRestorer).
    Result<QStringList> restore(const QString& snapshotId, const QString& target,
                                const QStringList& includePaths,
                                const CancellationToken& cancellation);

    /// Applies the retention policy; with \p dryRun nothing is deleted.
    Result<QVector<ForgetGroup>> forget(const RetentionPolicy& policy, bool dryRun, bool prune);

    Result<VerifyResult> check(const QString& readDataSubset = QStringLiteral("5%"));

    Result<QVector<LockInfo>> listLocks();
    Status unlock(bool removeAll = false);

    // Parsers (exposed for unit tests)
    static Result<QVector<SnapshotInfo>> parseSnapshots(const QByteArray& json);
    static Result<BackupSummary> parseBackupLines(const QVector<QByteArray>& lines,
                                                  BackupProgress* lastProgress = nullptr);
    static Result<BackupProgress> parseBackupStatus(const QJsonObject& object);
    static Result<QVector<FileEntry>> parseListLines(const QVector<QByteArray>& lines);
    static Result<QVector<ForgetGroup>> parseForgetOutput(const QByteArray& json);
    static Result<VerifyResult> parseCheckLines(const QVector<QByteArray>& lines);
    static Result<QVector<LockInfo>> parseLockList(const QByteArray& output);

private:
    Result<QVector<QByteArray>> runCollect(const QStringList& arguments,
                                           const CancellationToken& cancellation, int timeoutMs,
                                           ResticOutcome* outcome,
                                           const std::function<void(const QByteArray&)>& onLine = {});

    QString executable_;
    QString repository_;
    QString passwordFile_;
    std::shared_ptr<ResticRunner> runner_;
};

/// Creates a 0600 temporary password file holding \p password, inside the Nesva
/// cache directory. The caller must remove it (ResticPasswordFile does).
Result<QString> createPasswordFile(const QByteArray& password);

} // namespace nesva::backup
