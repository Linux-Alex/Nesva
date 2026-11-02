// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/jobs/job_store.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSysInfo>
#include <QtCore/QVariant>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include <utility>

#include "nesva/core/process_utils.hpp"
#include "nesva/db/database.hpp"

namespace nesva::jobs {
namespace {

QString columnText(const QSqlQuery& query, int index) {
    return query.value(index).toString();
}

QString sanitisedLabel(const QString& candidate) {
    static const QRegularExpression invalid(
        QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1F]"));
    QString label = candidate;
    label.remove(invalid);
    label = label.trimmed();
    label.replace(QLatin1Char(' '), QLatin1Char('_'));
    while (label.startsWith(QLatin1Char('.'))) {
        label.remove(0, 1);
    }
    return label;
}

QString uniqueLabel(const QString& base, QStringList& used) {
    QString candidate = base.isEmpty() ? QStringLiteral("source") : base;
    int suffix = 2;
    while (used.contains(candidate)) {
        candidate = QStringLiteral("%1-%2").arg(base).arg(suffix++);
    }
    used.append(candidate);
    return candidate;
}

QString sourceLabelFor(const JobSource& source, QStringList& used) {
    if (!source.label.trimmed().isEmpty()) {
        return uniqueLabel(sanitisedLabel(source.label), used);
    }
    const QString name = QFileInfo(QDir::cleanPath(source.path)).fileName();
    return uniqueLabel(sanitisedLabel(name), used);
}

bool isUniqueConstraintError(const QSqlError& error) {
    return error.text().contains(QStringLiteral("UNIQUE constraint failed"), Qt::CaseInsensitive);
}

scan::PatternKind parsePatternKind(const QString& value) {
    if (value == QStringLiteral("regex")) {
        return scan::PatternKind::Regex;
    }
    if (value == QStringLiteral("literal")) {
        return scan::PatternKind::Literal;
    }
    return scan::PatternKind::Glob;
}

QString patternKindToSql(scan::PatternKind kind) {
    switch (kind) {
    case scan::PatternKind::Regex:
        return QStringLiteral("regex");
    case scan::PatternKind::Literal:
        return QStringLiteral("literal");
    case scan::PatternKind::Glob:
        break;
    }
    return QStringLiteral("glob");
}

RunCounters countersFromQuery(const QSqlQuery& query, int offset) {
    RunCounters counters;
    counters.filesTotal = query.value(offset).toLongLong();
    counters.filesTransferred = query.value(offset + 1).toLongLong();
    counters.filesSkipped = query.value(offset + 2).toLongLong();
    counters.filesFailed = query.value(offset + 3).toLongLong();
    counters.bytesTotal = query.value(offset + 4).toLongLong();
    counters.bytesTransferred = query.value(offset + 5).toLongLong();
    return counters;
}

} // namespace

JobStore::JobStore(db::Database& database) : database_(database) {}

// ---------------------------------------------------------------------------
// Storage providers
// ---------------------------------------------------------------------------

Result<QVector<StorageProviderRecord>> JobStore::listProviders() const {
    Result<QSqlQuery> query = database_.select(QStringLiteral(
        "SELECT id, name, provider_type, COALESCE(rclone_remote, ''), COALESCE(base_path, ''), "
        "status, COALESCE(status_detail, ''), COALESCE(last_checked_at, '') "
        "FROM storage_providers ORDER BY name"));
    if (!query) {
        return query.error();
    }

    QSqlQuery rows = std::move(query).take();
    QVector<StorageProviderRecord> providers;
    while (rows.next()) {
        StorageProviderRecord record;
        record.id = rows.value(0).toLongLong();
        record.name = columnText(rows, 1);
        record.providerType = columnText(rows, 2);
        record.rcloneRemote = columnText(rows, 3);
        record.basePath = columnText(rows, 4);
        record.status = columnText(rows, 5);
        record.statusDetail = columnText(rows, 6);
        record.lastCheckedAt = columnText(rows, 7);
        providers.append(record);
    }
    return providers;
}

Result<StorageProviderRecord> JobStore::getProvider(qint64 id) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral("SELECT id, name, provider_type, COALESCE(rclone_remote, ''), "
                       "COALESCE(base_path, ''), status, COALESCE(status_detail, ''), "
                       "COALESCE(last_checked_at, '') FROM storage_providers WHERE id = ?"),
        {QVariant(id)});
    if (!query) {
        return query.error();
    }
    QSqlQuery rows = std::move(query).take();
    if (!rows.next()) {
        return makeErrorValue(ErrorCode::NotFound, "storage provider not found",
                              QString::number(id).toStdString());
    }
    StorageProviderRecord record;
    record.id = rows.value(0).toLongLong();
    record.name = columnText(rows, 1);
    record.providerType = columnText(rows, 2);
    record.rcloneRemote = columnText(rows, 3);
    record.basePath = columnText(rows, 4);
    record.status = columnText(rows, 5);
    record.statusDetail = columnText(rows, 6);
    record.lastCheckedAt = columnText(rows, 7);
    return record;
}

Result<qint64> JobStore::upsertLocalProvider(const QString& name, const QString& rootPath) {
    const QString timestamp = db::Database::currentTimestampUtc();

    QSqlQuery upsert(database_.handle());
    upsert.prepare(QStringLiteral(
        "INSERT INTO storage_providers (name, provider_type, base_path, status, created_at, "
        "updated_at) VALUES (?, 'local', ?, 'unknown', ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET provider_type = 'local', base_path = excluded.base_path, "
        "rclone_remote = NULL, updated_at = excluded.updated_at"));
    upsert.addBindValue(name);
    upsert.addBindValue(QDir::cleanPath(rootPath));
    upsert.addBindValue(timestamp);
    upsert.addBindValue(timestamp);
    if (!upsert.exec()) {
        return makeErrorValue(ErrorCode::DatabaseError, "cannot store local provider",
                              upsert.lastError().text().toStdString());
    }

    Result<QSqlQuery> lookup = database_.select(
        QStringLiteral("SELECT id FROM storage_providers WHERE name = ?"), {QVariant(name)});
    if (!lookup) {
        return lookup.error();
    }
    QSqlQuery rows = std::move(lookup).take();
    if (!rows.next()) {
        return makeErrorValue(ErrorCode::DatabaseError, "provider row disappeared after upsert");
    }
    return rows.value(0).toLongLong();
}

Result<qint64> JobStore::upsertRcloneProvider(const QString& name, const QString& rcloneRemote,
                                              const QString& basePath) {
    const QString timestamp = db::Database::currentTimestampUtc();

    QSqlQuery upsert(database_.handle());
    upsert.prepare(QStringLiteral(
        "INSERT INTO storage_providers (name, provider_type, rclone_remote, base_path, status, "
        "created_at, updated_at) VALUES (?, 'rclone', ?, ?, 'unknown', ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET provider_type = 'rclone', "
        "rclone_remote = excluded.rclone_remote, base_path = excluded.base_path, "
        "updated_at = excluded.updated_at"));
    upsert.addBindValue(name);
    upsert.addBindValue(rcloneRemote);
    upsert.addBindValue(basePath);
    upsert.addBindValue(timestamp);
    upsert.addBindValue(timestamp);
    if (!upsert.exec()) {
        return makeErrorValue(ErrorCode::DatabaseError, "cannot store rclone provider",
                              upsert.lastError().text().toStdString());
    }

    Result<QSqlQuery> lookup = database_.select(
        QStringLiteral("SELECT id FROM storage_providers WHERE name = ?"), {QVariant(name)});
    if (!lookup) {
        return lookup.error();
    }
    QSqlQuery rows = std::move(lookup).take();
    if (!rows.next()) {
        return makeErrorValue(ErrorCode::DatabaseError, "provider row disappeared after upsert");
    }
    return rows.value(0).toLongLong();
}

Status JobStore::updateProviderStatus(qint64 id, const QString& status, const QString& detail) {
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "UPDATE storage_providers SET status = ?, status_detail = ?, last_checked_at = ?, "
        "updated_at = ? WHERE id = ?"));
    query.addBindValue(status);
    query.addBindValue(detail.isEmpty() ? QVariant() : QVariant(detail));
    query.addBindValue(db::Database::currentTimestampUtc());
    query.addBindValue(db::Database::currentTimestampUtc());
    query.addBindValue(id);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot update provider status",
                         query.lastError().text().toStdString());
    }
    return Status();
}

Status JobStore::deleteProvider(qint64 id) {
    Result<QSqlQuery> used = database_.select(
        QStringLiteral("SELECT COUNT(*) FROM backup_jobs WHERE storage_provider_id = ?"),
        {QVariant(id)});
    if (!used) {
        return used.error();
    }
    {
        QSqlQuery rows = std::move(used).take();
        if (rows.next() && rows.value(0).toInt() > 0) {
            return makeError(ErrorCode::InvalidArgument,
                             "the storage provider is still used by a job");
        }
    }

    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral("DELETE FROM storage_providers WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot delete the storage provider",
                         query.lastError().text().toStdString());
    }
    if (query.numRowsAffected() == 0) {
        return makeError(ErrorCode::NotFound, "storage provider not found");
    }
    return Status();
}

// ---------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------

Result<QVector<ArchiveJob>> JobStore::listJobs() const {
    Result<QSqlQuery> query = database_.select(QStringLiteral(
        "SELECT id, name, mode, storage_provider_id, COALESCE(destination_path, ''), enabled, "
        "COALESCE(bandwidth_limit_kib, 0), verification_options_json, retention_policy_json "
        "FROM backup_jobs ORDER BY name"));
    if (!query) {
        return query.error();
    }

    QSqlQuery rows = std::move(query).take();
    QVector<ArchiveJob> jobs;
    while (rows.next()) {
        ArchiveJob job;
        job.id = rows.value(0).toLongLong();
        job.name = columnText(rows, 1);
        job.mode = columnText(rows, 2);
        if (!rows.value(3).isNull()) {
            job.storageProviderId = rows.value(3).toLongLong();
        }
        job.destinationPath = columnText(rows, 4);
        job.enabled = rows.value(5).toBool();
        job.bandwidthLimitKiB = rows.value(6).toInt();
        const QJsonObject verification =
            QJsonDocument::fromJson(rows.value(7).toString().toUtf8()).object();
        job.verifyChecksum = verification.value(QStringLiteral("checksum")).toBool(true);
        job.checkAfterBackup =
            verification.value(QStringLiteral("check_after_backup")).toBool(false);
        job.retention = backup::retentionFromJson(
            QJsonDocument::fromJson(rows.value(8).toString().toUtf8()).object());
        jobs.append(job);
    }

    for (ArchiveJob& job : jobs) {
        Result<QSqlQuery> sources = database_.select(
            QStringLiteral("SELECT id, path, COALESCE(label, '') FROM backup_sources "
                           "WHERE job_id = ? ORDER BY id"),
            {QVariant(job.id)});
        if (!sources) {
            return sources.error();
        }
        QSqlQuery sourceRows = std::move(sources).take();
        while (sourceRows.next()) {
            JobSource source;
            source.id = sourceRows.value(0).toLongLong();
            source.path = sourceRows.value(1).toString();
            source.label = sourceRows.value(2).toString();
            job.sources.append(source);
        }

        Result<QSqlQuery> exclusions = database_.select(
            QStringLiteral("SELECT pattern, match_kind FROM backup_exclusions "
                           "WHERE job_id = ? ORDER BY id"),
            {QVariant(job.id)});
        if (!exclusions) {
            return exclusions.error();
        }
        QSqlQuery exclusionRows = std::move(exclusions).take();
        while (exclusionRows.next()) {
            job.exclusions.append(
                {exclusionRows.value(0).toString(), parsePatternKind(exclusionRows.value(1).toString())});
        }
    }

    return jobs;
}

Result<ArchiveJob> JobStore::getJob(qint64 id) const {
    Result<QVector<ArchiveJob>> jobs = listJobs();
    if (!jobs) {
        return jobs.error();
    }
    for (const ArchiveJob& job : jobs.value()) {
        if (job.id == id) {
            return job;
        }
    }
    return makeErrorValue(ErrorCode::NotFound, "job not found", QString::number(id).toStdString());
}

Result<qint64> JobStore::createJob(const ArchiveJob& job) {
    if (!job.isValid()) {
        return makeErrorValue(ErrorCode::InvalidArgument, "job needs a name and at least one source");
    }
    if (job.mode != QStringLiteral("archive") && job.mode != QStringLiteral("backup")) {
        return makeErrorValue(ErrorCode::InvalidArgument, "unknown job mode: " + job.mode.toStdString());
    }
    if (job.mode == QStringLiteral("backup") && job.destinationPath.trimmed().isEmpty()) {
        return makeErrorValue(ErrorCode::InvalidArgument,
                              "a backup job needs a Restic repository location");
    }

    const QString timestamp = db::Database::currentTimestampUtc();
    db::Transaction transaction(database_);

    QJsonObject verification;
    verification.insert(QStringLiteral("checksum"), job.verifyChecksum);
    verification.insert(QStringLiteral("check_after_backup"), job.checkAfterBackup);

    QSqlQuery insert(database_.handle());
    insert.prepare(QStringLiteral(
        "INSERT INTO backup_jobs (name, mode, storage_provider_id, destination_path, enabled, "
        "bandwidth_limit_kib, verification_options_json, retention_policy_json, created_at, "
        "updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(job.name.trimmed());
    insert.addBindValue(job.mode);
    if (job.storageProviderId) {
        insert.addBindValue(*job.storageProviderId);
    } else {
        insert.addBindValue(QVariant());
    }
    insert.addBindValue(job.destinationPath.trimmed());
    insert.addBindValue(job.enabled ? 1 : 0);
    insert.addBindValue(job.bandwidthLimitKiB > 0 ? job.bandwidthLimitKiB : QVariant());
    insert.addBindValue(QString::fromUtf8(QJsonDocument(verification).toJson(QJsonDocument::Compact)));
    insert.addBindValue(QString::fromUtf8(
        QJsonDocument(backup::retentionToJson(job.retention)).toJson(QJsonDocument::Compact)));
    insert.addBindValue(timestamp);
    insert.addBindValue(timestamp);
    if (!insert.exec()) {
        return makeErrorValue(ErrorCode::DatabaseError, "cannot create job",
                              insert.lastError().text().toStdString());
    }
    const qint64 jobId = insert.lastInsertId().toLongLong();

    QStringList usedLabels;
    for (const JobSource& source : job.sources) {
        QSqlQuery sourceInsert(database_.handle());
        sourceInsert.prepare(QStringLiteral(
            "INSERT INTO backup_sources (job_id, path, label, created_at) VALUES (?, ?, ?, ?)"));
        sourceInsert.addBindValue(jobId);
        sourceInsert.addBindValue(QDir::cleanPath(source.path));
        sourceInsert.addBindValue(sourceLabelFor(source, usedLabels));
        sourceInsert.addBindValue(timestamp);
        if (!sourceInsert.exec()) {
            return makeErrorValue(ErrorCode::DatabaseError, "cannot create job source",
                                  sourceInsert.lastError().text().toStdString());
        }
    }

    for (const scan::ExclusionRule& rule : job.exclusions) {
        if (rule.pattern.trimmed().isEmpty()) {
            continue;
        }
        QSqlQuery exclusionInsert(database_.handle());
        exclusionInsert.prepare(QStringLiteral(
            "INSERT INTO backup_exclusions (job_id, pattern, match_kind, created_at) "
            "VALUES (?, ?, ?, ?)"));
        exclusionInsert.addBindValue(jobId);
        exclusionInsert.addBindValue(rule.pattern.trimmed());
        exclusionInsert.addBindValue(patternKindToSql(rule.kind));
        exclusionInsert.addBindValue(timestamp);
        if (!exclusionInsert.exec()) {
            return makeErrorValue(ErrorCode::DatabaseError, "cannot create job exclusion",
                                  exclusionInsert.lastError().text().toStdString());
        }
    }

    const Status committed = transaction.commit();
    if (!committed) {
        return committed.error();
    }
    return jobId;
}

Status JobStore::updateJob(const ArchiveJob& job) {
    if (job.id <= 0) {
        return makeError(ErrorCode::InvalidArgument, "job id is required");
    }
    if (!job.isValid()) {
        return makeError(ErrorCode::InvalidArgument, "job needs a name and at least one source");
    }
    if (job.mode != QStringLiteral("archive") && job.mode != QStringLiteral("backup")) {
        return makeError(ErrorCode::InvalidArgument, "unknown job mode: " + job.mode.toStdString());
    }

    const QString timestamp = db::Database::currentTimestampUtc();
    db::Transaction transaction(database_);

    QJsonObject verification;
    verification.insert(QStringLiteral("checksum"), job.verifyChecksum);
    verification.insert(QStringLiteral("check_after_backup"), job.checkAfterBackup);

    QSqlQuery update(database_.handle());
    update.prepare(QStringLiteral(
        "UPDATE backup_jobs SET name = ?, mode = ?, storage_provider_id = ?, "
        "destination_path = ?, enabled = ?, bandwidth_limit_kib = ?, "
        "verification_options_json = ?, retention_policy_json = ?, updated_at = ? WHERE id = ?"));
    update.addBindValue(job.name.trimmed());
    update.addBindValue(job.mode);
    if (job.storageProviderId) {
        update.addBindValue(*job.storageProviderId);
    } else {
        update.addBindValue(QVariant());
    }
    update.addBindValue(job.destinationPath.trimmed());
    update.addBindValue(job.enabled ? 1 : 0);
    update.addBindValue(job.bandwidthLimitKiB > 0 ? job.bandwidthLimitKiB : QVariant());
    update.addBindValue(QString::fromUtf8(QJsonDocument(verification).toJson(QJsonDocument::Compact)));
    update.addBindValue(QString::fromUtf8(
        QJsonDocument(backup::retentionToJson(job.retention)).toJson(QJsonDocument::Compact)));
    update.addBindValue(timestamp);
    update.addBindValue(job.id);
    if (!update.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot update job",
                         update.lastError().text().toStdString());
    }

    QSqlQuery clearSources(database_.handle());
    clearSources.prepare(QStringLiteral("DELETE FROM backup_sources WHERE job_id = ?"));
    clearSources.addBindValue(job.id);
    if (!clearSources.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot replace job sources",
                         clearSources.lastError().text().toStdString());
    }

    QStringList usedLabels;
    for (const JobSource& source : job.sources) {
        QSqlQuery sourceInsert(database_.handle());
        sourceInsert.prepare(QStringLiteral(
            "INSERT INTO backup_sources (job_id, path, label, created_at) VALUES (?, ?, ?, ?)"));
        sourceInsert.addBindValue(job.id);
        sourceInsert.addBindValue(QDir::cleanPath(source.path));
        sourceInsert.addBindValue(sourceLabelFor(source, usedLabels));
        sourceInsert.addBindValue(timestamp);
        if (!sourceInsert.exec()) {
            return makeError(ErrorCode::DatabaseError, "cannot update job source",
                             sourceInsert.lastError().text().toStdString());
        }
    }

    QSqlQuery clearExclusions(database_.handle());
    clearExclusions.prepare(QStringLiteral("DELETE FROM backup_exclusions WHERE job_id = ?"));
    clearExclusions.addBindValue(job.id);
    if (!clearExclusions.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot replace job exclusions",
                         clearExclusions.lastError().text().toStdString());
    }

    for (const scan::ExclusionRule& rule : job.exclusions) {
        if (rule.pattern.trimmed().isEmpty()) {
            continue;
        }
        QSqlQuery exclusionInsert(database_.handle());
        exclusionInsert.prepare(QStringLiteral(
            "INSERT INTO backup_exclusions (job_id, pattern, match_kind, created_at) "
            "VALUES (?, ?, ?, ?)"));
        exclusionInsert.addBindValue(job.id);
        exclusionInsert.addBindValue(rule.pattern.trimmed());
        exclusionInsert.addBindValue(patternKindToSql(rule.kind));
        exclusionInsert.addBindValue(timestamp);
        if (!exclusionInsert.exec()) {
            return makeError(ErrorCode::DatabaseError, "cannot update job exclusion",
                             exclusionInsert.lastError().text().toStdString());
        }
    }

    return transaction.commit();
}

Status JobStore::deleteJob(qint64 id) {
    Result<bool> running = isJobRunning(id);
    if (!running) {
        return running.error();
    }
    if (running.value()) {
        return makeError(ErrorCode::InvalidArgument,
                         "cannot delete a job while it is running; cancel the run first");
    }

    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral("DELETE FROM backup_jobs WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot delete job",
                         query.lastError().text().toStdString());
    }
    if (query.numRowsAffected() == 0) {
        return makeError(ErrorCode::NotFound, "job not found", QString::number(id).toStdString());
    }
    return Status();
}

// ---------------------------------------------------------------------------
// Runs
// ---------------------------------------------------------------------------

Result<qint64> JobStore::beginRun(qint64 jobId, const QString& trigger) {
    const QString timestamp = db::Database::currentTimestampUtc();

    QSqlQuery insert(database_.handle());
    insert.prepare(QStringLiteral(
        "INSERT INTO job_runs (job_id, run_number, trigger_kind, status, started_at, owner_pid, "
        "owner_host, created_at) VALUES (?, (SELECT COALESCE(MAX(run_number), 0) + 1 "
        "FROM job_runs WHERE job_id = ?), ?, 'running', ?, ?, ?, ?)"));
    insert.addBindValue(jobId);
    insert.addBindValue(jobId);
    insert.addBindValue(trigger);
    insert.addBindValue(timestamp);
    insert.addBindValue(QCoreApplication::applicationPid());
    insert.addBindValue(QSysInfo::machineHostName());
    insert.addBindValue(timestamp);

    if (!insert.exec()) {
        if (isUniqueConstraintError(insert.lastError())) {
            return makeErrorValue(ErrorCode::AlreadyExists,
                                  "a run for this job is already in progress");
        }
        return makeErrorValue(ErrorCode::DatabaseError, "cannot start a job run",
                              insert.lastError().text().toStdString());
    }
    return insert.lastInsertId().toLongLong();
}

Status JobStore::updateRunCounters(qint64 runId, const RunCounters& counters) {
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "UPDATE job_runs SET files_total = ?, files_transferred = ?, files_skipped = ?, "
        "files_failed = ?, bytes_total = ?, bytes_transferred = ? WHERE id = ?"));
    query.addBindValue(counters.filesTotal);
    query.addBindValue(counters.filesTransferred);
    query.addBindValue(counters.filesSkipped);
    query.addBindValue(counters.filesFailed);
    query.addBindValue(counters.bytesTotal);
    query.addBindValue(counters.bytesTransferred);
    query.addBindValue(runId);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot update run counters",
                         query.lastError().text().toStdString());
    }
    return Status();
}

Status JobStore::finishRun(qint64 runId, const QString& status, const RunCounters& counters,
                           const std::optional<Error>& error) {
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "UPDATE job_runs SET status = ?, finished_at = ?, files_total = ?, "
        "files_transferred = ?, files_skipped = ?, files_failed = ?, bytes_total = ?, "
        "bytes_transferred = ?, error_code = ?, error_message = ? WHERE id = ?"));
    query.addBindValue(status);
    query.addBindValue(db::Database::currentTimestampUtc());
    query.addBindValue(counters.filesTotal);
    query.addBindValue(counters.filesTransferred);
    query.addBindValue(counters.filesSkipped);
    query.addBindValue(counters.filesFailed);
    query.addBindValue(counters.bytesTotal);
    query.addBindValue(counters.bytesTransferred);
    query.addBindValue(error ? QVariant(QString::fromUtf8(toToken(error->code()))) : QVariant());
    query.addBindValue(error ? QVariant(QString::fromStdString(error->toDisplayString()))
                             : QVariant());
    query.addBindValue(runId);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot finish run",
                         query.lastError().text().toStdString());
    }
    return Status();
}

Result<int> JobStore::recoverStaleRuns() {
    // Another machine's run cannot be verified from here, so those rows are
    // left alone; only runs that claim to belong to this host are inspected.
    Result<QSqlQuery> query = database_.select(QStringLiteral(
        "SELECT id, COALESCE(owner_pid, 0), COALESCE(owner_host, '') FROM job_runs "
        "WHERE status IN ('running', 'queued')"));
    if (!query) {
        return query.error();
    }

    QVector<qint64> stale;
    const QString host = QSysInfo::machineHostName();
    {
        QSqlQuery rows = std::move(query).take();
        while (rows.next()) {
            const qint64 runId = rows.value(0).toLongLong();
            const qint64 ownerPid = rows.value(1).toLongLong();
            const QString ownerHost = rows.value(2).toString();
            const bool sameHost = ownerHost.isEmpty() || ownerHost == host;
            if (!sameHost) {
                continue;
            }
            if (ownerPid <= 0 || !isProcessAlive(ownerPid)) {
                stale.append(runId);
            }
        }
    }

    if (stale.isEmpty()) {
        return 0;
    }

    db::Transaction transaction(database_);
    const QString timestamp = db::Database::currentTimestampUtc();
    for (const qint64 runId : stale) {
        QSqlQuery update(database_.handle());
        update.prepare(QStringLiteral(
            "UPDATE job_runs SET status = 'interrupted', finished_at = ?, "
            "error_code = 'interrupted', error_message = ? WHERE id = ? AND status = 'running'"));
        update.addBindValue(timestamp);
        update.addBindValue(QStringLiteral(
            "the process that owned this run is no longer running (recovered at startup)"));
        update.addBindValue(runId);
        if (!update.exec()) {
            return makeErrorValue(ErrorCode::DatabaseError, "cannot recover stale run",
                                  update.lastError().text().toStdString());
        }
    }

    const Status committed = transaction.commit();
    if (!committed) {
        return committed.error();
    }
    return static_cast<int>(stale.size());
}

Result<QVector<RunSummary>> JobStore::listRuns(int limit, std::optional<qint64> jobId) const {
    QString sql = QStringLiteral(
        "SELECT r.id, r.job_id, j.name, r.run_number, r.trigger_kind, r.status, "
        "COALESCE(r.started_at, ''), COALESCE(r.finished_at, ''), r.files_total, "
        "r.files_transferred, r.files_skipped, r.files_failed, r.bytes_total, "
        "r.bytes_transferred, COALESCE(r.error_code, ''), COALESCE(r.error_message, '') "
        "FROM job_runs r JOIN backup_jobs j ON j.id = r.job_id ");
    QVariantList bindings;
    if (jobId) {
        sql += QStringLiteral("WHERE r.job_id = ? ");
        bindings.append(QVariant(*jobId));
    }
    sql += QStringLiteral("ORDER BY r.id DESC LIMIT ?");
    bindings.append(QVariant(limit));

    Result<QSqlQuery> query = database_.select(sql, bindings);
    if (!query) {
        return query.error();
    }

    QSqlQuery rows = std::move(query).take();
    QVector<RunSummary> runs;
    while (rows.next()) {
        RunSummary run;
        run.id = rows.value(0).toLongLong();
        run.jobId = rows.value(1).toLongLong();
        run.jobName = columnText(rows, 2);
        run.runNumber = rows.value(3).toLongLong();
        run.trigger = columnText(rows, 4);
        run.status = columnText(rows, 5);
        run.startedAt = columnText(rows, 6);
        run.finishedAt = columnText(rows, 7);
        run.counters = countersFromQuery(rows, 8);
        run.errorCode = columnText(rows, 14);
        run.errorMessage = columnText(rows, 15);
        runs.append(run);
    }
    return runs;
}

Result<RunSummary> JobStore::getRun(qint64 runId) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral(
            "SELECT r.id, r.job_id, j.name, r.run_number, r.trigger_kind, r.status, "
            "COALESCE(r.started_at, ''), COALESCE(r.finished_at, ''), r.files_total, "
            "r.files_transferred, r.files_skipped, r.files_failed, r.bytes_total, "
            "r.bytes_transferred, COALESCE(r.error_code, ''), COALESCE(r.error_message, '') "
            "FROM job_runs r JOIN backup_jobs j ON j.id = r.job_id WHERE r.id = ?"),
        {QVariant(runId)});
    if (!query) {
        return query.error();
    }
    QSqlQuery rows = std::move(query).take();
    if (!rows.next()) {
        return makeErrorValue(ErrorCode::NotFound, "run not found", QString::number(runId).toStdString());
    }
    RunSummary run;
    run.id = rows.value(0).toLongLong();
    run.jobId = rows.value(1).toLongLong();
    run.jobName = columnText(rows, 2);
    run.runNumber = rows.value(3).toLongLong();
    run.trigger = columnText(rows, 4);
    run.status = columnText(rows, 5);
    run.startedAt = columnText(rows, 6);
    run.finishedAt = columnText(rows, 7);
    run.counters = countersFromQuery(rows, 8);
    run.errorCode = columnText(rows, 14);
    run.errorMessage = columnText(rows, 15);
    return run;
}

Result<bool> JobStore::isJobRunning(qint64 jobId) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral("SELECT 1 FROM job_runs WHERE job_id = ? AND status = 'running'"),
        {QVariant(jobId)});
    if (!query) {
        return query.error();
    }
    QSqlQuery rows = std::move(query).take();
    return rows.next();
}

Status JobStore::appendJobEvent(qint64 jobId, std::optional<qint64> runId, LogLevel level,
                                const QString& category, const QString& message,
                                const QJsonObject& data) {
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "INSERT INTO job_events (job_id, job_run_id, level, category, message, data_json, "
        "occurred_at) VALUES (?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(jobId);
    if (runId) {
        query.addBindValue(*runId);
    } else {
        query.addBindValue(QVariant());
    }
    query.addBindValue(QString::fromUtf8(toToken(level)));
    query.addBindValue(category);
    query.addBindValue(message);
    query.addBindValue(data.isEmpty() ? QVariant()
                                      : QVariant(QString::fromUtf8(
                                            QJsonDocument(data).toJson(QJsonDocument::Compact))));
    query.addBindValue(db::Database::currentTimestampUtc());
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot append job event",
                         query.lastError().text().toStdString());
    }
    return Status();
}

// ---------------------------------------------------------------------------
// File manifest
// ---------------------------------------------------------------------------

Result<QMap<QString, ManifestSnapshot>> JobStore::lastSuccessfulManifest(qint64 jobId) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral(
            "SELECT m.relative_path, m.size_bytes, COALESCE(m.mtime_utc, ''), COALESCE(m.sha256, '') "
            "FROM file_manifest m "
            "WHERE m.job_id = ? AND m.state IN ('transferred', 'verified', 'skipped') "
            "AND m.job_run_id = (SELECT id FROM job_runs WHERE job_id = ? AND status = 'succeeded' "
            "ORDER BY id DESC LIMIT 1)"),
        {QVariant(jobId), QVariant(jobId)});
    if (!query) {
        return query.error();
    }

    QSqlQuery rows = std::move(query).take();
    QMap<QString, ManifestSnapshot> manifest;
    while (rows.next()) {
        ManifestSnapshot snapshot;
        snapshot.sizeBytes = rows.value(1).toLongLong();
        snapshot.modifiedUtc = rows.value(2).toString();
        snapshot.sha256 = rows.value(3).toString();
        manifest.insert(rows.value(0).toString(), snapshot);
    }
    return manifest;
}

Result<QVector<ManifestRecord>> JobStore::latestManifest(qint64 jobId) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral(
            "SELECT m.relative_path, m.source_path, m.size_bytes, COALESCE(m.mtime_utc, ''), "
            "COALESCE(m.sha256, ''), m.state, COALESCE(m.skip_reason, '') "
            "FROM file_manifest m WHERE m.job_id = ? AND m.job_run_id = "
            "(SELECT MAX(job_run_id) FROM file_manifest WHERE job_id = ?) "
            "ORDER BY m.relative_path"),
        {QVariant(jobId), QVariant(jobId)});
    if (!query) {
        return query.error();
    }

    QSqlQuery rows = std::move(query).take();
    QVector<ManifestRecord> records;
    while (rows.next()) {
        ManifestRecord record;
        record.relativePath = rows.value(0).toString();
        record.sourcePath = rows.value(1).toString();
        record.sizeBytes = rows.value(2).toLongLong();
        record.modifiedUtc = rows.value(3).toString();
        record.sha256 = rows.value(4).toString();
        record.state = rows.value(5).toString();
        record.skipReason = rows.value(6).toString();
        records.append(record);
    }
    return records;
}

Status JobStore::recordManifestEntry(qint64 jobId, qint64 runId, const ManifestRecord& record) {
    const QString timestamp = db::Database::currentTimestampUtc();

    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "INSERT INTO file_manifest (job_id, job_run_id, relative_path, source_path, remote_path, "
        "size_bytes, mtime_utc, sha256, state, skip_reason, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(job_run_id, relative_path) DO UPDATE SET state = excluded.state, "
        "sha256 = excluded.sha256, skip_reason = excluded.skip_reason, "
        "size_bytes = excluded.size_bytes, updated_at = excluded.updated_at"));
    query.addBindValue(jobId);
    query.addBindValue(runId);
    query.addBindValue(record.relativePath);
    query.addBindValue(record.sourcePath);
    query.addBindValue(record.relativePath);
    query.addBindValue(record.sizeBytes);
    query.addBindValue(record.modifiedUtc.isEmpty() ? QVariant() : QVariant(record.modifiedUtc));
    query.addBindValue(record.sha256.isEmpty() ? QVariant() : QVariant(record.sha256));
    query.addBindValue(record.state);
    query.addBindValue(record.skipReason.isEmpty() ? QVariant() : QVariant(record.skipReason));
    query.addBindValue(timestamp);
    query.addBindValue(timestamp);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot record manifest entry",
                         query.lastError().text().toStdString());
    }
    return Status();
}

Status JobStore::recordArchiveObject(qint64 jobId, qint64 runId, const QString& remotePath,
                                     qint64 sizeBytes, const QString& sha256,
                                     const QString& state) {
    const QString timestamp = db::Database::currentTimestampUtc();

    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "INSERT INTO archive_objects (job_id, job_run_id, remote_path, size_bytes, sha256, "
        "uploaded_at, verified_at, state) VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(job_id, remote_path) DO UPDATE SET job_run_id = excluded.job_run_id, "
        "size_bytes = excluded.size_bytes, sha256 = excluded.sha256, "
        "uploaded_at = excluded.uploaded_at, verified_at = excluded.verified_at, "
        "state = excluded.state"));
    query.addBindValue(jobId);
    query.addBindValue(runId);
    query.addBindValue(remotePath);
    query.addBindValue(sizeBytes);
    query.addBindValue(sha256.isEmpty() ? QVariant() : QVariant(sha256));
    query.addBindValue(timestamp);
    query.addBindValue(sha256.isEmpty() ? QVariant() : QVariant(timestamp));
    query.addBindValue(state);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot record archive object",
                         query.lastError().text().toStdString());
    }
    return Status();
}

// ---------------------------------------------------------------------------
// Schedules
// ---------------------------------------------------------------------------

namespace {

QString timeToText(const QTime& time) {
    return time.toString(QStringLiteral("HH:mm"));
}

QTime timeFromText(const QString& text) {
    const QTime parsed = QTime::fromString(text, QStringLiteral("HH:mm"));
    return parsed.isValid() ? parsed : QTime(2, 30);
}

std::optional<QDateTime> optionalUtc(const QVariant& value) {
    if (value.isNull()) {
        return std::nullopt;
    }
    const QString text = value.toString();
    if (text.isEmpty()) {
        return std::nullopt;
    }
    const QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    return parsed.isValid() ? std::optional<QDateTime>(parsed.toUTC()) : std::nullopt;
}

QVariant utcVariant(const std::optional<QDateTime>& value) {
    if (!value.has_value()) {
        return QVariant();
    }
    return value->toUTC().toString(Qt::ISODateWithMs);
}

} // namespace

Result<std::optional<ScheduleRecord>> JobStore::getSchedule(qint64 jobId) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral(
            "SELECT trigger_kind, COALESCE(time_of_day, '02:30'), COALESCE(day_of_week, 1), "
            "COALESCE(day_of_month, 1), timezone, COALESCE(max_duration_seconds, 0), "
            "retry_max_attempts, retry_backoff_seconds, catch_up_missed, next_run_at, last_run_at, "
            "COALESCE(retry_attempt_count, 0), retry_next_at "
            "FROM job_schedules WHERE job_id = ?"),
        {QVariant(jobId)});
    if (!query) {
        return query.error();
    }

    QSqlQuery rows = std::move(query).take();
    if (!rows.next()) {
        return std::optional<ScheduleRecord>{};
    }

    ScheduleRecord record;
    record.spec.kind = sched::triggerKindFromString(rows.value(0).toString())
                           .value_or(sched::TriggerKind::Manual);
    record.spec.timeOfDay = timeFromText(rows.value(1).toString());
    record.spec.dayOfWeek = rows.value(2).toInt();
    record.spec.dayOfMonth = rows.value(3).toInt();
    record.spec.timezone = rows.value(4).toString().toLatin1();
    record.spec.maxDurationSeconds = rows.value(5).toInt();
    record.spec.retryMaxAttempts = rows.value(6).toInt();
    record.spec.retryBackoffSeconds = rows.value(7).toInt();
    record.spec.catchUpMissed = rows.value(8).toBool();
    record.state.nextRunUtc = optionalUtc(rows.value(9));
    record.state.lastRunUtc = optionalUtc(rows.value(10));
    record.state.retryAttemptCount = rows.value(11).toInt();
    record.state.retryNextUtc = optionalUtc(rows.value(12));
    return std::optional<ScheduleRecord>(record);
}

Status JobStore::upsertSchedule(qint64 jobId, const sched::ScheduleSpec& spec) {
    if (const auto invalid = spec.validate()) {
        return makeError(ErrorCode::InvalidArgument, invalid->toStdString());
    }

    const QString timestamp = db::Database::currentTimestampUtc();
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "INSERT INTO job_schedules (job_id, trigger_kind, time_of_day, day_of_week, day_of_month, "
        "timezone, max_duration_seconds, retry_max_attempts, retry_backoff_seconds, catch_up_missed, "
        "created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(job_id) DO UPDATE SET trigger_kind = excluded.trigger_kind, "
        "time_of_day = excluded.time_of_day, day_of_week = excluded.day_of_week, "
        "day_of_month = excluded.day_of_month, timezone = excluded.timezone, "
        "max_duration_seconds = excluded.max_duration_seconds, "
        "retry_max_attempts = excluded.retry_max_attempts, "
        "retry_backoff_seconds = excluded.retry_backoff_seconds, "
        "catch_up_missed = excluded.catch_up_missed, updated_at = excluded.updated_at"));
    query.addBindValue(jobId);
    query.addBindValue(sched::toToken(spec.kind));
    query.addBindValue(timeToText(spec.timeOfDay));
    query.addBindValue(spec.dayOfWeek);
    query.addBindValue(spec.dayOfMonth);
    query.addBindValue(QString::fromLatin1(spec.timezone));
    query.addBindValue(spec.maxDurationSeconds > 0 ? spec.maxDurationSeconds : QVariant());
    query.addBindValue(spec.retryMaxAttempts);
    query.addBindValue(spec.retryBackoffSeconds);
    query.addBindValue(spec.catchUpMissed ? 1 : 0);
    query.addBindValue(timestamp);
    query.addBindValue(timestamp);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot store the schedule",
                         query.lastError().text().toStdString());
    }

    // Give the schedule a concrete next slot immediately, so the dashboard and
    // the CLI show a real time even before the agent has run a tick.
    Result<std::optional<ScheduleRecord>> stored = getSchedule(jobId);
    if (stored && stored.value().has_value() && spec.isRecurring() &&
        !stored.value()->state.nextRunUtc.has_value()) {
        sched::ScheduleState state = stored.value()->state;
        state.nextRunUtc = sched::computeNextRun(spec, QDateTime::currentDateTimeUtc());
        return updateScheduleState(jobId, state);
    }
    return Status();
}

Status JobStore::deleteSchedule(qint64 jobId) {
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral("DELETE FROM job_schedules WHERE job_id = ?"));
    query.addBindValue(jobId);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot delete the schedule",
                         query.lastError().text().toStdString());
    }
    return Status();
}

Status JobStore::updateScheduleState(qint64 jobId, const sched::ScheduleState& state) {
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "UPDATE job_schedules SET next_run_at = ?, last_run_at = ?, retry_attempt_count = ?, "
        "retry_next_at = ?, updated_at = ? WHERE job_id = ?"));
    query.addBindValue(utcVariant(state.nextRunUtc));
    query.addBindValue(utcVariant(state.lastRunUtc));
    query.addBindValue(state.retryAttemptCount);
    query.addBindValue(utcVariant(state.retryNextUtc));
    query.addBindValue(db::Database::currentTimestampUtc());
    query.addBindValue(jobId);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot update the schedule state",
                         query.lastError().text().toStdString());
    }
    return Status();
}

Result<QVector<QPair<qint64, ScheduleRecord>>> JobStore::listSchedules() const {
    Result<QSqlQuery> query = database_.select(QStringLiteral(
        "SELECT s.job_id, s.trigger_kind, COALESCE(s.time_of_day, '02:30'), "
        "COALESCE(s.day_of_week, 1), COALESCE(s.day_of_month, 1), s.timezone, "
        "COALESCE(s.max_duration_seconds, 0), s.retry_max_attempts, s.retry_backoff_seconds, "
        "s.catch_up_missed, s.next_run_at, s.last_run_at, COALESCE(s.retry_attempt_count, 0), "
        "s.retry_next_at "
        "FROM job_schedules s JOIN backup_jobs j ON j.id = s.job_id "
        "WHERE j.enabled = 1 ORDER BY s.job_id"));
    if (!query) {
        return query.error();
    }

    QSqlQuery rows = std::move(query).take();
    QVector<QPair<qint64, ScheduleRecord>> schedules;
    while (rows.next()) {
        ScheduleRecord record;
        record.spec.kind = sched::triggerKindFromString(rows.value(1).toString())
                               .value_or(sched::TriggerKind::Manual);
        record.spec.timeOfDay = timeFromText(rows.value(2).toString());
        record.spec.dayOfWeek = rows.value(3).toInt();
        record.spec.dayOfMonth = rows.value(4).toInt();
        record.spec.timezone = rows.value(5).toString().toLatin1();
        record.spec.maxDurationSeconds = rows.value(6).toInt();
        record.spec.retryMaxAttempts = rows.value(7).toInt();
        record.spec.retryBackoffSeconds = rows.value(8).toInt();
        record.spec.catchUpMissed = rows.value(9).toBool();
        record.state.nextRunUtc = optionalUtc(rows.value(10));
        record.state.lastRunUtc = optionalUtc(rows.value(11));
        record.state.retryAttemptCount = rows.value(12).toInt();
        record.state.retryNextUtc = optionalUtc(rows.value(13));
        schedules.append({rows.value(0).toLongLong(), record});
    }
    return schedules;
}

Result<QVector<QPair<qint64, QDateTime>>> JobStore::upcomingRuns(int limit) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral("SELECT job_id, next_run_at FROM job_schedules "
                       "WHERE next_run_at IS NOT NULL ORDER BY next_run_at LIMIT ?"),
        {QVariant(limit)});
    if (!query) {
        return query.error();
    }

    QSqlQuery rows = std::move(query).take();
    QVector<QPair<qint64, QDateTime>> upcoming;
    while (rows.next()) {
        const std::optional<QDateTime> slot = optionalUtc(rows.value(1));
        if (slot) {
            upcoming.append({rows.value(0).toLongLong(), *slot});
        }
    }
    return upcoming;
}

// ---------------------------------------------------------------------------
// Restic snapshots
// ---------------------------------------------------------------------------

Status JobStore::recordSnapshot(qint64 jobId, qint64 runId, const SnapshotRecord& snapshot) {
    if (snapshot.snapshotId.trimmed().isEmpty()) {
        return makeError(ErrorCode::InvalidArgument, "snapshot id must not be empty");
    }
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "INSERT INTO backup_snapshots (job_id, snapshot_id, hostname, repository_path, created_at, "
        "tags_json, paths_json, size_bytes, files_processed) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(job_id, snapshot_id) DO UPDATE SET hostname = excluded.hostname, "
        "repository_path = excluded.repository_path, size_bytes = excluded.size_bytes, "
        "files_processed = excluded.files_processed"));
    query.addBindValue(jobId);
    query.addBindValue(snapshot.snapshotId);
    query.addBindValue(snapshot.hostname);
    query.addBindValue(snapshot.repositoryPath);
    query.addBindValue(snapshot.createdAt.isEmpty() ? db::Database::currentTimestampUtc()
                                                    : snapshot.createdAt);
    query.addBindValue(QStringLiteral("[]"));
    query.addBindValue(QStringLiteral("[]"));
    query.addBindValue(snapshot.sizeBytes);
    query.addBindValue(snapshot.filesProcessed >= 0 ? snapshot.filesProcessed : QVariant());
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot record the snapshot",
                         query.lastError().text().toStdString());
    }
    Q_UNUSED(runId);
    return Status();
}

Result<QVector<JobStore::SnapshotRecord>> JobStore::listSnapshots(qint64 jobId, int limit) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral(
            "SELECT id, snapshot_id, COALESCE(hostname, ''), COALESCE(repository_path, ''), "
            "COALESCE(created_at, ''), COALESCE(size_bytes, -1), "
            "COALESCE(files_processed, -1) FROM backup_snapshots "
            "WHERE job_id = ? ORDER BY created_at DESC LIMIT ?"),
        {QVariant(jobId), QVariant(limit)});
    if (!query) {
        return query.error();
    }
    QSqlQuery rows = std::move(query).take();
    QVector<SnapshotRecord> snapshots;
    while (rows.next()) {
        SnapshotRecord record;
        record.id = rows.value(0).toLongLong();
        record.snapshotId = rows.value(1).toString();
        record.hostname = rows.value(2).toString();
        record.repositoryPath = rows.value(3).toString();
        record.createdAt = rows.value(4).toString();
        record.sizeBytes = rows.value(5).toLongLong();
        record.filesProcessed = rows.value(6).toInt();
        snapshots.append(record);
    }
    return snapshots;
}

Result<QVector<JobStore::SnapshotRecord>> JobStore::allSnapshots(int limit) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral(
            "SELECT id, snapshot_id, COALESCE(hostname, ''), COALESCE(repository_path, ''), "
            "COALESCE(created_at, ''), COALESCE(size_bytes, -1), "
            "COALESCE(files_processed, -1) FROM backup_snapshots "
            "ORDER BY created_at DESC LIMIT ?"),
        {QVariant(limit)});
    if (!query) {
        return query.error();
    }
    QSqlQuery rows = std::move(query).take();
    QVector<SnapshotRecord> snapshots;
    while (rows.next()) {
        SnapshotRecord record;
        record.id = rows.value(0).toLongLong();
        record.snapshotId = rows.value(1).toString();
        record.hostname = rows.value(2).toString();
        record.repositoryPath = rows.value(3).toString();
        record.createdAt = rows.value(4).toString();
        record.sizeBytes = rows.value(5).toLongLong();
        record.filesProcessed = rows.value(6).toInt();
        snapshots.append(record);
    }
    return snapshots;
}

} // namespace nesva::jobs
