// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>

#include <memory>
#include <optional>

#include "nesva/core/cancellation.hpp"
#include "nesva/core/result.hpp"
#include "nesva/jobs/archive_restore.hpp"
#include "nesva/jobs/archive_runner.hpp"
#include "nesva/jobs/job_store.hpp"

namespace nesva::db {
class Database;
}

namespace nesva::backup {
class ResticRunner;
}

namespace nesva::jobs {

/// Where the repository password comes from.
enum class PasswordSource {
    Keyring, ///< OS credential store (preferred)
    File,    ///< explicit file with restrictive permissions (fallback)
};

struct PasswordOptions {
    PasswordSource source = PasswordSource::Keyring;
    QString keyringService = QStringLiteral("nesva");
    QString filePath; ///< used when source == File
};

/// Resolves the repository password for a job.
///
/// The keyring account is derived from the repository location, so moving a
/// job to another repository does not silently reuse the old secret.
Result<QByteArray> loadRepositoryPassword(const QString& repository,
                                          const PasswordOptions& options);

/// Stores the password for a repository in the configured credential store.
Status storeRepositoryPassword(const QString& repository, const PasswordOptions& options,
                               const QByteArray& password);

/// Keyring account name used for a repository (stable, not secret).
QString repositoryAccount(const QString& repository);

/// Runs a Restic backup job: resolves the password, executes restic, records the
/// run, its counters and the produced snapshot.
class BackupRunner {
public:
    BackupRunner(db::Database& database, RunProgressCallback progress = {},
                 std::shared_ptr<backup::ResticRunner> resticRunner = nullptr);

    RunReport run(const ArchiveJob& job, const RunOptions& options,
                  const CancellationToken& cancellation, const QString& trigger,
                  const QString& resticExecutable, const PasswordOptions& passwordOptions);

private:
    db::Database& database_;
    JobStore store_;
    RunProgressCallback progress_;
    std::shared_ptr<backup::ResticRunner> resticRunner_;
};

/// Restores a snapshot produced by a backup job.
///
/// restic restores into a staging directory first; the result is then merged
/// into the destination with the requested conflict policy, so an existing file
/// is never overwritten silently.
class BackupRestorer {
public:
    struct Request {
        ArchiveJob job;
        QString resticExecutable;
        QString snapshotId; ///< full or short id, or "latest"
        QString destinationDirectory;
        ConflictPolicy policy = ConflictPolicy::Skip;
        PasswordOptions passwordOptions;
    };

    struct Report {
        QString status = QStringLiteral("failed");
        int restored = 0;
        int skipped = 0;
        int renamed = 0;
        int failed = 0;
        qint64 bytesRestored = 0;
        QStringList errors;

        bool succeeded() const { return status == QStringLiteral("succeeded"); }
    };

    explicit BackupRestorer(db::Database& database);

    Report restore(const Request& request, const CancellationToken& cancellation);

private:
    db::Database& database_;
};

} // namespace nesva::jobs
