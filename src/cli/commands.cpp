// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "commands.hpp"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QStorageInfo>
#include <QtCore/QTemporaryFile>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include <cstdio>
#include <iostream>
#include <string>
#include <algorithm>
#include <utility>

#include "command_line.hpp"
#include "nesva/config/settings_store.hpp"
#include "nesva/core/app_info.hpp"
#include "nesva/core/external_tool.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/core/paths.hpp"
#include "nesva/db/database.hpp"
#include "nesva/jobs/archive_restore.hpp"
#include "nesva/jobs/archive_runner.hpp"
#include "nesva/agent/client.hpp"
#include "nesva/backup/restic.hpp"
#include "nesva/jobs/backup_runner.hpp"
#include "nesva/jobs/job_store.hpp"
#include "nesva/scheduler/schedule.hpp"
#include "nesva/providers/provider_factory.hpp"

namespace nesva::cli {
namespace {

constexpr qint64 kMinimumFreeBytes = 1024LL * 1024LL * 1024LL; // 1 GiB

/// Collects command output and renders it as text or JSON exactly once, so the
/// two representations cannot drift apart.
class Output {
public:
    explicit Output(bool json) : json_(json) {}

    void add(const QString& key, const QJsonValue& value) { object_.insert(key, value); }
    void line(const QString& text) { lines_.append(text); }
    void table(const QStringList& headers, const QList<QStringList>& rows);

    int flush(FILE* stream, int exitCode);
    int flushError(const Error& error, int exitCode);

private:
    static QString renderTable(const QStringList& headers, const QList<QStringList>& rows);

    bool json_ = false;
    QJsonObject object_;
    QStringList lines_;
};

QString Output::renderTable(const QStringList& headers, const QList<QStringList>& rows) {
    QList<QStringList> allRows;
    allRows.reserve(rows.size() + 1);
    allRows.append(headers);
    for (const QStringList& row : rows) {
        allRows.append(row);
    }

    QVector<int> widths(headers.size(), 0);
    for (const QStringList& row : allRows) {
        for (int column = 0; column < row.size() && column < widths.size(); ++column) {
            widths[column] = std::max(widths[column], static_cast<int>(row.at(column).size()));
        }
    }

    QStringList rendered;
    rendered.reserve(allRows.size() + 1);

    auto renderRow = [&](const QStringList& row) {
        QStringList cells;
        cells.reserve(widths.size());
        for (int column = 0; column < widths.size(); ++column) {
            const QString cell = column < row.size() ? row.at(column) : QString();
            cells.append(column + 1 == widths.size() ? cell : cell.leftJustified(widths.at(column)));
        }
        return cells.join(QStringLiteral("  ")).trimmed();
    };

    rendered.append(renderRow(allRows.constFirst()));

    QStringList underline;
    underline.reserve(widths.size());
    for (const int width : widths) {
        underline.append(QString(width, QLatin1Char('-')));
    }
    rendered.append(underline.join(QStringLiteral("  ")).trimmed());

    for (int rowIndex = 1; rowIndex < allRows.size(); ++rowIndex) {
        rendered.append(renderRow(allRows.at(rowIndex)));
    }

    return rendered.join(QLatin1Char('\n'));
}

void Output::table(const QStringList& headers, const QList<QStringList>& rows) {
    if (json_) {
        QJsonArray array;
        for (const QStringList& row : rows) {
            QJsonObject entry;
            for (int column = 0; column < row.size() && column < headers.size(); ++column) {
                entry.insert(headers.at(column), row.at(column));
            }
            array.append(entry);
        }
        object_.insert(QStringLiteral("items"), array);
        return;
    }
    if (rows.isEmpty()) {
        return;
    }
    lines_.append(renderTable(headers, rows));
}

int Output::flush(FILE* stream, int exitCode) {
    if (json_) {
        const QByteArray document =
            QJsonDocument(object_).toJson(QJsonDocument::Indented);
        std::fwrite(document.constData(), 1, static_cast<std::size_t>(document.size()), stream);
        std::fflush(stream);
        return exitCode;
    }
    const QByteArray text = lines_.join(QLatin1Char('\n')).toUtf8() + '\n';
    std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stream);
    std::fflush(stream);
    return exitCode;
}

int Output::flushError(const Error& error, int exitCode) {
    if (json_) {
        QJsonObject errorObject;
        errorObject.insert(QStringLiteral("code"), QString::fromUtf8(toToken(error.code())));
        errorObject.insert(QStringLiteral("message"), QString::fromStdString(error.message()));
        if (!error.detail().empty()) {
            errorObject.insert(QStringLiteral("detail"), QString::fromStdString(error.detail()));
        }
        object_.insert(QStringLiteral("error"), errorObject);
        object_.insert(QStringLiteral("ok"), false);
        return flush(stdout, exitCode);
    }
    const QString text = QStringLiteral("error: %1").arg(
        QString::fromStdString(error.toDisplayString()));
    const QByteArray utf8 = text.toUtf8() + '\n';
    std::fwrite(utf8.constData(), 1, static_cast<std::size_t>(utf8.size()), stderr);
    std::fflush(stderr);
    return exitCode;
}

int exitCodeFor(const ErrorCode code) {
    switch (code) {
    case ErrorCode::NotFound:
        return kExitNotFound;
    case ErrorCode::NotImplemented:
        return kExitNotImplemented;
    case ErrorCode::InvalidArgument:
    case ErrorCode::ConfigurationError:
        return kExitUsage;
    default:
        return kExitFailure;
    }
}

// ---------------------------------------------------------------------------
// Shared context
// ---------------------------------------------------------------------------

struct Context {
    db::Database database;
    QString logFile;
};

Result<QString> prepareContext(Context& context, const Options& options) {
    const Status directories = Paths::ensureBaseDirectories();
    if (!directories) {
        return directories.error();
    }

    LoggingOptions logging;
    logging.logDirectory = Paths::logsDirectory();
    logging.fileLevel = LogLevel::Debug;
    logging.consoleEnabled = !options.json && !options.quiet;
    logging.consoleLevel = options.verbose ? LogLevel::Debug : LogLevel::Warn;
    logging.consoleToStandardError = true;

    const Result<QString> logFile = initialiseLogging(logging);
    if (!logFile) {
        return logFile.error();
    }

    const Status opened = context.database.open(Paths::databasePath());
    if (!opened) {
        return opened.error();
    }

    const Status migrated = context.database.migrate();
    if (!migrated) {
        return migrated.error();
    }

    logInfo("cli", QStringLiteral("command invoked"),
            QJsonObject{{QStringLiteral("command"), options.command},
                        {QStringLiteral("subcommand"), options.subcommand}});
    return logFile;
}

bool directoryIsWritable(const QString& path, QString* detail) {
    QDir directory(path);
    if (!directory.exists()) {
        if (detail != nullptr) {
            *detail = QStringLiteral("directory does not exist");
        }
        return false;
    }
    QTemporaryFile probe(directory.absoluteFilePath(QStringLiteral(".nesva-write-probe-XXXXXX")));
    if (!probe.open()) {
        if (detail != nullptr) {
            *detail = probe.errorString();
        }
        return false;
    }
    probe.write("nesva");
    probe.flush();
    const QString name = probe.fileName();
    probe.close();
    const bool removed = QFile::remove(name);
    if (!removed && detail != nullptr) {
        *detail = QStringLiteral("probe file could not be removed");
    }
    return removed;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int commandStatus(Context& context, const Options& options) {
    Output output(options.json);

    Result<int> schemaVersion = context.database.schemaVersion();
    if (!schemaVersion) {
        return output.flushError(schemaVersion.error(), kExitFailure);
    }

    int jobCount = 0;
    int enabledJobs = 0;
    int storageCount = 0;
    QJsonObject lastRun;
    bool hasLastRun = false;

    {
        Result<QSqlQuery> query = context.database.select(
            QStringLiteral("SELECT COUNT(*), "
                           "COALESCE(SUM(CASE WHEN enabled = 1 THEN 1 ELSE 0 END), 0) "
                           "FROM backup_jobs"));
        if (!query) {
            return output.flushError(query.error(), kExitFailure);
        }
        QSqlQuery rows = std::move(query).take();
        if (rows.next()) {
            jobCount = rows.value(0).toInt();
            enabledJobs = rows.value(1).toInt();
        }
    }
    {
        Result<QSqlQuery> query = context.database.select(
            QStringLiteral("SELECT COUNT(*) FROM storage_providers"));
        if (!query) {
            return output.flushError(query.error(), kExitFailure);
        }
        QSqlQuery rows = std::move(query).take();
        if (rows.next()) {
            storageCount = rows.value(0).toInt();
        }
    }
    {
        Result<QSqlQuery> query = context.database.select(QStringLiteral(
            "SELECT j.name, r.status, COALESCE(r.started_at, ''), COALESCE(r.finished_at, '') "
            "FROM job_runs r JOIN backup_jobs j ON j.id = r.job_id "
            "ORDER BY r.id DESC LIMIT 1"));
        if (!query) {
            return output.flushError(query.error(), kExitFailure);
        }
        QSqlQuery rows = std::move(query).take();
        if (rows.next()) {
            hasLastRun = true;
            lastRun.insert(QStringLiteral("job"), rows.value(0).toString());
            lastRun.insert(QStringLiteral("status"), rows.value(1).toString());
            lastRun.insert(QStringLiteral("started_at"), rows.value(2).toString());
            lastRun.insert(QStringLiteral("finished_at"), rows.value(3).toString());
        }
    }

    QJsonObject application = ApplicationInfo::toJson();
    QJsonObject paths;
    paths.insert(QStringLiteral("data_root"), Paths::dataRoot());
    paths.insert(QStringLiteral("database"), context.database.filePath());
    paths.insert(QStringLiteral("logs"), Paths::logsDirectory());
    paths.insert(QStringLiteral("log_file"), context.logFile);
    paths.insert(QStringLiteral("resolution"), Paths::describeResolution());

    QJsonObject databaseObject;
    databaseObject.insert(QStringLiteral("path"), context.database.filePath());
    databaseObject.insert(QStringLiteral("schema_version"), schemaVersion.value());
    databaseObject.insert(QStringLiteral("latest_schema_version"), db::Database::latestSchemaVersion());

    QJsonObject jobsObject;
    jobsObject.insert(QStringLiteral("configured"), jobCount);
    jobsObject.insert(QStringLiteral("enabled"), enabledJobs);

    output.add(QStringLiteral("ok"), true);
    output.add(QStringLiteral("application"), application);
    output.add(QStringLiteral("paths"), paths);
    output.add(QStringLiteral("database"), databaseObject);
    output.add(QStringLiteral("jobs"), jobsObject);
    output.add(QStringLiteral("last_run"), hasLastRun ? QJsonValue(lastRun) : QJsonValue());
    output.add(QStringLiteral("storage_providers"), storageCount);

    output.line(QStringLiteral("%1 (commit %2, Qt %3)")
                    .arg(ApplicationInfo::displayVersion(), ApplicationInfo::gitCommit(),
                         ApplicationInfo::qtVersion()));
    output.line(QStringLiteral("Platform:    %1").arg(ApplicationInfo::platformDescription()));
    output.line(QStringLiteral("Data root:   %1").arg(Paths::describeResolution()));
    output.line(QStringLiteral("Database:    %1 (schema %2 of %3)")
                    .arg(context.database.filePath())
                    .arg(schemaVersion.value())
                    .arg(db::Database::latestSchemaVersion()));
    output.line(QStringLiteral("Log file:    %1").arg(context.logFile));
    output.line(QStringLiteral("Jobs:        %1 configured, %2 enabled")
                    .arg(jobCount)
                    .arg(enabledJobs));
    output.line(QStringLiteral("Storage:     %1 configured").arg(storageCount));
    if (hasLastRun) {
        output.line(QStringLiteral("Last run:    %1 -> %2 (%3)")
                        .arg(lastRun.value(QStringLiteral("job")).toString(),
                             lastRun.value(QStringLiteral("status")).toString(),
                             lastRun.value(QStringLiteral("started_at")).toString()));
    } else {
        output.line(QStringLiteral("Last run:    none recorded"));
    }
    return output.flush(stdout, kExitSuccess);
}

jobs::PasswordOptions passwordOptionsFromFlags(const Options& options, Context& context);

struct CheckResult {
    QString name;
    QString status; // "ok", "warning", "error"
    QString detail;
};

/// Job plus the storage provider row it references (when any).
struct JobContext {
    jobs::ArchiveJob job;
    std::optional<jobs::StorageProviderRecord> provider;
};

Result<JobContext> loadJobContext(jobs::JobStore& store, qint64 jobId) {
    Result<jobs::ArchiveJob> job = store.getJob(jobId);
    if (!job) {
        return job.error();
    }
    JobContext context;
    context.job = job.value();
    if (context.job.storageProviderId) {
        Result<jobs::StorageProviderRecord> provider = store.getProvider(*context.job.storageProviderId);
        if (!provider) {
            return provider.error();
        }
        context.provider = provider.value();
    }
    return context;
}

void printProgressLine(const jobs::RunProgress& progress, bool json) {
    if (json) {
        return;
    }
    QString line = QStringLiteral("[%1] %2")
                       .arg(progress.filesProcessed)
                       .arg(progress.filesTotal);
    if (!progress.currentFile.isEmpty()) {
        line += QStringLiteral(" ") + progress.currentFile;
    }
    if (!progress.message.isEmpty()) {
        line += QStringLiteral(" — ") + progress.message;
    }
    std::fprintf(stderr, "%s\n", qPrintable(line));
    std::fflush(stderr);
}

CheckResult makeCheck(const QString& name, const QString& status, const QString& detail) {
    return CheckResult{name, status, detail};
}

int commandDoctor(Context& context, const Options& options) {
    Output output(options.json);
    config::SettingsStore settings(context.database);
    QList<CheckResult> checks;

    checks.append(makeCheck(QStringLiteral("application"),
                            QStringLiteral("ok"),
                            QStringLiteral("%1, Qt %2, %3")
                                .arg(ApplicationInfo::displayVersion(),
                                     ApplicationInfo::qtVersion(),
                                     ApplicationInfo::platformDescription())));

    QString detail;
    const bool dataWritable = directoryIsWritable(Paths::dataRoot(), &detail);
    checks.append(makeCheck(QStringLiteral("data directory"),
                            dataWritable ? QStringLiteral("ok") : QStringLiteral("error"),
                            dataWritable ? Paths::dataRoot() : detail));

    const bool logsWritable = directoryIsWritable(Paths::logsDirectory(), &detail);
    checks.append(makeCheck(QStringLiteral("log directory"),
                            logsWritable ? QStringLiteral("ok") : QStringLiteral("error"),
                            logsWritable ? Paths::logsDirectory() : detail));

    QString integrityDetail;
    const Status integrity = context.database.checkIntegrity(&integrityDetail);
    checks.append(makeCheck(QStringLiteral("database integrity"),
                            integrity ? QStringLiteral("ok") : QStringLiteral("error"),
                            integrity ? context.database.filePath()
                                      : QString::fromStdString(integrity.error().toDisplayString())));

    const Result<int> schemaVersion = context.database.schemaVersion();
    const int latest = db::Database::latestSchemaVersion();
    if (!schemaVersion) {
        checks.append(makeCheck(QStringLiteral("schema version"), QStringLiteral("error"),
                                QString::fromStdString(schemaVersion.error().toDisplayString())));
    } else {
        const bool upToDate = schemaVersion.value() == latest;
        checks.append(makeCheck(QStringLiteral("schema version"),
                                upToDate ? QStringLiteral("ok") : QStringLiteral("error"),
                                QStringLiteral("%1 of %2").arg(schemaVersion.value()).arg(latest)));
    }

    const QStorageInfo storage(Paths::dataRoot());
    if (storage.isValid() && storage.isReady()) {
        const qint64 available = storage.bytesAvailable();
        const bool enough = available >= kMinimumFreeBytes;
        checks.append(makeCheck(
            QStringLiteral("free disk space"),
            enough ? QStringLiteral("ok") : QStringLiteral("warning"),
            QStringLiteral("%1 MiB available on %2")
                .arg(available / (1024 * 1024))
                .arg(storage.rootPath())));
    } else {
        checks.append(makeCheck(QStringLiteral("free disk space"), QStringLiteral("warning"),
                                QStringLiteral("storage information unavailable")));
    }

    struct ToolCheck {
        const char* settingKey;
        QString displayName;
    };
    const QList<ToolCheck> tools = {
        {config::keys::kRcloneBinaryPath, QStringLiteral("rclone")},
        {config::keys::kResticBinaryPath, QStringLiteral("restic")},
    };

    for (const ToolCheck& tool : tools) {
        const Result<QString> configured =
            settings.getString(QString::fromUtf8(tool.settingKey), tool.displayName);
        QString executable = tool.displayName;
        if (configured && !configured.value().isEmpty()) {
            executable = configured.value();
        }

        const Result<ExternalToolInfo> probe =
            probeExternalTool(executable, {QStringLiteral("version")});
        if (probe) {
            checks.append(makeCheck(tool.displayName, QStringLiteral("ok"),
                                    QStringLiteral("%1 (%2)").arg(probe.value().executable,
                                                                   probe.value().versionText)));
        } else {
            checks.append(makeCheck(
                tool.displayName, QStringLiteral("warning"),
                QStringLiteral("%1 - cloud/backup operations that require it are unavailable")
                    .arg(QString::fromStdString(probe.error().toDisplayString()))));
        }
    }

    int errors = 0;
    int warnings = 0;
    QJsonArray checksArray;
    for (const CheckResult& check : checks) {
        if (check.status == QStringLiteral("error")) {
            ++errors;
        } else if (check.status == QStringLiteral("warning")) {
            ++warnings;
        }
        QJsonObject entry;
        entry.insert(QStringLiteral("check"), check.name);
        entry.insert(QStringLiteral("status"), check.status);
        entry.insert(QStringLiteral("detail"), check.detail);
        checksArray.append(entry);
        output.line(QStringLiteral("[%1] %2: %3")
                        .arg(check.status.toUpper().leftJustified(7), check.name.leftJustified(18),
                             check.detail));
    }

    output.add(QStringLiteral("ok"), errors == 0);
    output.add(QStringLiteral("errors"), errors);
    output.add(QStringLiteral("warnings"), warnings);
    output.add(QStringLiteral("checks"), checksArray);
    output.line(QStringLiteral("%1 error(s), %2 warning(s)").arg(errors).arg(warnings));

    return output.flush(stdout, errors == 0 ? kExitSuccess : kExitFailure);
}

int commandJobsList(Context& context, const Options& options) {
    Output output(options.json);
    Result<QSqlQuery> query = context.database.select(QStringLiteral(
        "SELECT j.id, j.name, j.mode, j.enabled, COALESCE(j.destination_path, ''), "
        "COALESCE(sp.name, ''), "
        "(SELECT COUNT(*) FROM backup_sources s WHERE s.job_id = j.id), "
        "COALESCE((SELECT r.status FROM job_runs r WHERE r.job_id = j.id ORDER BY r.id DESC LIMIT 1), ''), "
        "COALESCE((SELECT r.finished_at FROM job_runs r WHERE r.job_id = j.id ORDER BY r.id DESC LIMIT 1), '') "
        "FROM backup_jobs j LEFT JOIN storage_providers sp ON sp.id = j.storage_provider_id "
        "ORDER BY j.name"));
    if (!query) {
        return output.flushError(query.error(), kExitFailure);
    }

    QSqlQuery rows = std::move(query).take();
    QList<QStringList> table;
    QJsonArray items;
    while (rows.next()) {
        const QString id = QString::number(rows.value(0).toLongLong());
        const QString name = rows.value(1).toString();
        const QString mode = rows.value(2).toString();
        const QString enabled = rows.value(3).toBool() ? QStringLiteral("yes") : QStringLiteral("no");
        const QString destination = rows.value(4).toString().isEmpty() ? rows.value(5).toString()
                                                                     : rows.value(4).toString();
        const QString sources = rows.value(6).toString();
        const QString lastStatus = rows.value(7).toString();
        const QString lastFinished = rows.value(8).toString();

        table.append({id, name, mode, enabled, sources, destination,
                      lastStatus.isEmpty() ? QStringLiteral("never") : lastStatus, lastFinished});

        QJsonObject item;
        item.insert(QStringLiteral("id"), rows.value(0).toLongLong());
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("mode"), mode);
        item.insert(QStringLiteral("enabled"), rows.value(3).toBool());
        item.insert(QStringLiteral("storage_provider"), rows.value(5).toString());
        item.insert(QStringLiteral("destination_path"), rows.value(4).toString());
        item.insert(QStringLiteral("source_count"), rows.value(6).toInt());
        item.insert(QStringLiteral("last_status"), lastStatus);
        item.insert(QStringLiteral("last_finished_at"), lastFinished);
        items.append(item);
    }

    output.add(QStringLiteral("ok"), true);
    output.add(QStringLiteral("count"), static_cast<int>(table.size()));
    output.table({QStringLiteral("ID"), QStringLiteral("NAME"), QStringLiteral("MODE"),
                  QStringLiteral("ENABLED"), QStringLiteral("SOURCES"), QStringLiteral("DESTINATION"),
                  QStringLiteral("LAST RUN"), QStringLiteral("FINISHED")},
                 table);
    if (table.isEmpty()) {
        output.line(QStringLiteral("No jobs configured yet."));
    }
    return output.flush(stdout, kExitSuccess);
}

int commandHistoryList(Context& context, const Options& options) {
    Output output(options.json);
    int limit = 25;
    if (options.flags.contains(QStringLiteral("limit"))) {
        bool ok = false;
        const int requested = options.flags.value(QStringLiteral("limit")).toInt(&ok);
        if (!ok || requested <= 0) {
            return output.flushError(
                makeErrorValue(ErrorCode::InvalidArgument, "--limit requires a positive integer"),
                kExitUsage);
        }
        limit = requested;
    }

    Result<QSqlQuery> query = context.database.select(
        QStringLiteral("SELECT r.id, j.name, r.run_number, r.status, "
                       "COALESCE(r.started_at, ''), COALESCE(r.finished_at, ''), "
                       "r.files_transferred, r.files_failed, r.bytes_transferred, "
                       "COALESCE(r.error_message, '') "
                       "FROM job_runs r JOIN backup_jobs j ON j.id = r.job_id "
                       "ORDER BY r.id DESC LIMIT ?"),
        {QVariant(limit)});
    if (!query) {
        return output.flushError(query.error(), kExitFailure);
    }

    QSqlQuery rows = std::move(query).take();
    QList<QStringList> table;
    while (rows.next()) {
        table.append({QString::number(rows.value(0).toLongLong()), rows.value(1).toString(),
                      QString::number(rows.value(2).toLongLong()), rows.value(3).toString(),
                      rows.value(4).toString(), rows.value(5).toString(),
                      QString::number(rows.value(6).toLongLong()),
                      QString::number(rows.value(9).toString().isEmpty() ? 0 : rows.value(7).toLongLong()),
                      rows.value(9).toString()});
    }

    output.add(QStringLiteral("ok"), true);
    output.add(QStringLiteral("count"), static_cast<int>(table.size()));
    output.table({QStringLiteral("RUN"), QStringLiteral("JOB"), QStringLiteral("NO"),
                  QStringLiteral("STATUS"), QStringLiteral("STARTED"), QStringLiteral("FINISHED"),
                  QStringLiteral("FILES"), QStringLiteral("FAILED"), QStringLiteral("ERROR")},
                 table);
    if (table.isEmpty()) {
        output.line(QStringLiteral("No job runs recorded yet."));
    }
    return output.flush(stdout, kExitSuccess);
}

int commandStorageList(Context& context, const Options& options) {
    Output output(options.json);
    Result<QSqlQuery> query = context.database.select(QStringLiteral(
        "SELECT id, name, provider_type, COALESCE(rclone_remote, ''), "
        "COALESCE(base_path, ''), status, COALESCE(last_checked_at, '') "
        "FROM storage_providers ORDER BY name"));
    if (!query) {
        return output.flushError(query.error(), kExitFailure);
    }

    QSqlQuery rows = std::move(query).take();
    QList<QStringList> table;
    while (rows.next()) {
        table.append({QString::number(rows.value(0).toLongLong()), rows.value(1).toString(),
                      rows.value(2).toString(), rows.value(3).toString(), rows.value(4).toString(),
                      rows.value(5).toString(), rows.value(6).toString()});
    }

    output.add(QStringLiteral("ok"), true);
    output.add(QStringLiteral("count"), static_cast<int>(table.size()));
    output.table({QStringLiteral("ID"), QStringLiteral("NAME"), QStringLiteral("TYPE"),
                  QStringLiteral("RCLONE REMOTE"), QStringLiteral("BASE PATH"),
                  QStringLiteral("STATUS"), QStringLiteral("LAST CHECKED")},
                 table);
    if (table.isEmpty()) {
        output.line(QStringLiteral("No storage providers configured yet."));
    }
    return output.flush(stdout, kExitSuccess);
}

int commandNotImplemented(const Options& options, const QString& what, const QString& phase) {
    Output output(options.json);
    const Error error(ErrorCode::NotImplemented, what.toStdString(),
                      QStringLiteral("planned for %1; nothing was changed").arg(phase).toStdString());
    output.line(QStringLiteral("Not implemented in this build: %1").arg(what));
    output.line(QStringLiteral("Planned delivery: %1. No job, run or storage state was modified.")
                    .arg(phase));
    return output.flushError(error, kExitNotImplemented);
}


int commandJobsRun(Context& context, const Options& options) {
    Output output(options.json);
    if (options.positional.isEmpty()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "jobs run requires a job id"), kExitUsage);
    }
    bool valid = false;
    const qint64 jobId = options.positional.constFirst().toLongLong(&valid);
    if (!valid) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "job id must be numeric"), kExitUsage);
    }

    jobs::JobStore store(context.database);
    Result<JobContext> loaded = loadJobContext(store, jobId);
    if (!loaded) {
        return output.flushError(loaded.error(),
                                 loaded.error().code() == ErrorCode::NotFound ? kExitNotFound
                                                                              : kExitFailure);
    }

    jobs::RunOptions runOptions;
    runOptions.dryRun = options.flags.contains(QStringLiteral("dry-run"));
    runOptions.forceFullTransfer = options.flags.contains(QStringLiteral("force"));
    if (options.flags.contains(QStringLiteral("retries"))) {
        bool ok = false;
        const int retries = options.flags.value(QStringLiteral("retries")).toInt(&ok);
        if (!ok || retries < 0 || retries > 20) {
            return output.flushError(
                makeErrorValue(ErrorCode::InvalidArgument, "--retries must be between 0 and 20"),
                kExitUsage);
        }
        runOptions.maxAttempts = retries + 1;
    }

    config::SettingsStore settings(context.database);
    const QString rcloneExecutable =
        settings.getString(QString::fromUtf8(config::keys::kRcloneBinaryPath), QStringLiteral("rclone"))
            .valueOr(QStringLiteral("rclone"));

    const CancellationToken cancellation;
    const auto progressCallback = [quiet = options.quiet, json = options.json](
                                      const jobs::RunProgress& progress) {
        if (!quiet) {
            printProgressLine(progress, json);
        }
    };

    jobs::RunReport report;
    if (loaded.value().job.isBackup()) {
        const QString resticExecutable =
            settings.getString(QString::fromUtf8(config::keys::kResticBinaryPath),
                               QStringLiteral("restic"))
                .valueOr(QStringLiteral("restic"));
        jobs::BackupRunner backupRunner(context.database, progressCallback);
        report = backupRunner.run(loaded.value().job, runOptions, cancellation,
                                  QStringLiteral("cli"), resticExecutable,
                                  passwordOptionsFromFlags(options, context));
    } else {
        jobs::ArchiveRunner runner(context.database, progressCallback);
        report = runner.run(loaded.value().job, loaded.value().provider, runOptions, cancellation,
                            QStringLiteral("cli"), rcloneExecutable);
    }

    QJsonObject summary;
    summary.insert(QStringLiteral("run_id"), report.runId);
    summary.insert(QStringLiteral("job"), report.jobName);
    summary.insert(QStringLiteral("status"), report.status);
    summary.insert(QStringLiteral("dry_run"), report.dryRun);
    summary.insert(QStringLiteral("files_total"), report.counters.filesTotal);
    summary.insert(QStringLiteral("files_transferred"), report.counters.filesTransferred);
    summary.insert(QStringLiteral("files_skipped"), report.counters.filesSkipped);
    summary.insert(QStringLiteral("files_failed"), report.counters.filesFailed);
    summary.insert(QStringLiteral("bytes_transferred"), report.counters.bytesTransferred);
    QJsonArray errors;
    for (const QString& error : report.errors) {
        errors.append(error);
    }
    summary.insert(QStringLiteral("errors"), errors);

    output.add(QStringLiteral("ok"), report.succeeded());
    output.add(QStringLiteral("result"), summary);
    output.line(QStringLiteral("Job: %1").arg(report.jobName));
    output.line(QStringLiteral("Status: %1%2")
                    .arg(report.status, report.dryRun ? QStringLiteral(" (dry run)") : QString()));
    output.line(QStringLiteral("Transferred: %1 file(s), %2 byte(s)")
                    .arg(report.counters.filesTransferred)
                    .arg(report.counters.bytesTransferred));
    output.line(QStringLiteral("Unchanged: %1 file(s)").arg(report.counters.filesSkipped));
    output.line(QStringLiteral("Failed: %1 file(s)").arg(report.counters.filesFailed));
    for (const QString& error : report.errors) {
        output.line(QStringLiteral("  error: %1").arg(error));
    }

    if (report.status == QStringLiteral("succeeded")) {
        return output.flush(stdout, kExitSuccess);
    }
    if (report.status == QStringLiteral("cancelled")) {
        return output.flush(stdout, kExitFailure);
    }
    return output.flush(stdout, kExitFailure);
}

int commandJobsCancel(Context& context, const Options& options) {
    Output output(options.json);
    if (options.positional.isEmpty()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "jobs cancel requires a job id"), kExitUsage);
    }
    bool valid = false;
    const qint64 jobId = options.positional.constFirst().toLongLong(&valid);
    if (!valid) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "job id must be numeric"), kExitUsage);
    }

    jobs::JobStore store(context.database);
    Result<jobs::ArchiveJob> job = store.getJob(jobId);
    if (!job) {
        return output.flushError(job.error(), kExitNotFound);
    }

    Result<bool> running = store.isJobRunning(jobId);
    if (!running) {
        return output.flushError(running.error(), kExitFailure);
    }
    if (!running.value()) {
        return output.flushError(makeErrorValue(ErrorCode::NotFound,
                                                "no run in progress for this job"),
                                 kExitNotFound);
    }

    // A run owned by a live process cannot be cancelled from here; only the
    // process that started it can do that (the desktop app, or the agent in a
    // later phase). Runs whose owner is gone are recovered instead.
    Result<int> recovered = store.recoverStaleRuns();
    if (!recovered) {
        return output.flushError(recovered.error(), kExitFailure);
    }
    if (recovered.value() > 0) {
        output.add(QStringLiteral("ok"), true);
        output.add(QStringLiteral("recovered"), recovered.value());
        output.line(QStringLiteral("Marked %1 abandoned run(s) of job '%2' as interrupted.")
                        .arg(recovered.value())
                        .arg(job.value().name));
        return output.flush(stdout, kExitSuccess);
    }

    return output.flushError(
        makeErrorValue(ErrorCode::Unsupported,
                       "the run is owned by another running process",
                       "cancel it from the Nesva window that started it"),
        kExitFailure);
}

int commandStorageTest(Context& context, const Options& options) {
    Output output(options.json);
    if (options.positional.isEmpty()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "storage test requires a storage id"),
            kExitUsage);
    }
    bool valid = false;
    const qint64 providerId = options.positional.constFirst().toLongLong(&valid);
    if (!valid) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "storage id must be numeric"), kExitUsage);
    }

    jobs::JobStore store(context.database);
    Result<jobs::StorageProviderRecord> provider = store.getProvider(providerId);
    if (!provider) {
        return output.flushError(provider.error(), kExitNotFound);
    }

    providers::ProviderSelection selection;
    selection.displayName = provider.value().name;
    if (provider.value().isRcloneBacked()) {
        selection.rcloneRemote = provider.value().rcloneRemote;
        selection.basePath = provider.value().basePath;
    } else {
        selection.localRoot = provider.value().basePath;
    }

    config::SettingsStore settings(context.database);
    const QString rcloneExecutable =
        settings.getString(QString::fromUtf8(config::keys::kRcloneBinaryPath), QStringLiteral("rclone"))
            .valueOr(QStringLiteral("rclone"));

    Result<std::unique_ptr<providers::StorageProvider>> created =
        providers::createProvider(selection, rcloneExecutable);
    if (!created) {
        store.updateProviderStatus(providerId, QStringLiteral("error"),
                                   QString::fromStdString(created.error().message()));
        return output.flushError(created.error(), kExitFailure);
    }

    const Status status = created.value()->testConnection();
    store.updateProviderStatus(
        providerId, status.ok() ? QStringLiteral("ok") : QStringLiteral("error"),
        status.ok() ? QString() : QString::fromStdString(status.error().toDisplayString()));

    output.add(QStringLiteral("ok"), status.ok());
    output.add(QStringLiteral("provider"), provider.value().name);
    output.add(QStringLiteral("location"), created.value()->location());
    if (status.ok()) {
        output.line(QStringLiteral("Provider '%1' is reachable at %2")
                        .arg(provider.value().name, created.value()->location()));
        return output.flush(stdout, kExitSuccess);
    }
    output.line(QStringLiteral("Provider '%1' is not usable: %2")
                    .arg(provider.value().name,
                         QString::fromStdString(status.error().toDisplayString())));
    return output.flush(stdout, kExitFailure);
}

int commandArchiveList(Context& context, const Options& options) {
    Output output(options.json);
    if (options.positional.isEmpty()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "archive list requires a job id"),
            kExitUsage);
    }
    bool valid = false;
    const qint64 jobId = options.positional.constFirst().toLongLong(&valid);
    if (!valid) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "job id must be numeric"), kExitUsage);
    }

    jobs::JobStore store(context.database);
    Result<JobContext> loaded = loadJobContext(store, jobId);
    if (!loaded) {
        return output.flushError(loaded.error(),
                                 loaded.error().code() == ErrorCode::NotFound ? kExitNotFound
                                                                              : kExitFailure);
    }

    config::SettingsStore settings(context.database);
    const QString rcloneExecutable =
        settings.getString(QString::fromUtf8(config::keys::kRcloneBinaryPath), QStringLiteral("rclone"))
            .valueOr(QStringLiteral("rclone"));

    Result<jobs::ResolvedDestination> resolved =
        jobs::resolveDestination(loaded.value().job, loaded.value().provider, rcloneExecutable);
    if (!resolved) {
        return output.flushError(resolved.error(), kExitFailure);
    }
    jobs::ResolvedDestination destination = std::move(resolved).take();

    Result<QVector<providers::RemoteEntry>> listing = destination.provider->listFiles(QString(), true);
    if (!listing) {
        return output.flushError(listing.error(), kExitFailure);
    }

    Result<QMap<QString, jobs::ManifestSnapshot>> manifest = store.lastSuccessfulManifest(jobId);
    const QMap<QString, jobs::ManifestSnapshot> hashes =
        manifest ? manifest.value() : QMap<QString, jobs::ManifestSnapshot>();

    QList<QStringList> table;
    for (const providers::RemoteEntry& entry : listing.value()) {
        if (entry.isDirectory) {
            continue;
        }
        const auto hash = hashes.constFind(entry.relativePath);
        table.append({entry.relativePath, QString::number(entry.sizeBytes),
                      hash != hashes.constEnd() ? hash->sha256.left(16) : QStringLiteral("-"),
                      entry.modifiedUtc.toUTC().toString(Qt::ISODate)});
    }

    output.add(QStringLiteral("ok"), true);
    output.add(QStringLiteral("job"), loaded.value().job.name);
    output.add(QStringLiteral("location"), destination.provider->location());
    output.add(QStringLiteral("count"), static_cast<int>(table.size()));
    output.table({QStringLiteral("PATH"), QStringLiteral("BYTES"), QStringLiteral("SHA-256"),
                  QStringLiteral("MODIFIED")},
                 table);
    if (table.isEmpty()) {
        output.line(QStringLiteral("The archive is empty."));
    }
    return output.flush(stdout, kExitSuccess);
}

int commandVerify(Context& context, const Options& options) {
    Output output(options.json);
    if (options.positional.isEmpty()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "verify requires a job id"), kExitUsage);
    }
    bool valid = false;
    const qint64 jobId = options.positional.constFirst().toLongLong(&valid);
    if (!valid) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "job id must be numeric"), kExitUsage);
    }

    jobs::JobStore store(context.database);
    Result<JobContext> loaded = loadJobContext(store, jobId);
    if (!loaded) {
        return output.flushError(loaded.error(),
                                 loaded.error().code() == ErrorCode::NotFound ? kExitNotFound
                                                                              : kExitFailure);
    }
    if (loaded.value().job.mode != QStringLiteral("archive")) {
        return output.flushError(
            makeErrorValue(ErrorCode::NotImplemented,
                           "verification of Restic repositories is delivered in Phase 4"),
            kExitNotImplemented);
    }

    config::SettingsStore settings(context.database);
    const QString rcloneExecutable =
        settings.getString(QString::fromUtf8(config::keys::kRcloneBinaryPath), QStringLiteral("rclone"))
            .valueOr(QStringLiteral("rclone"));

    Result<jobs::ResolvedDestination> resolved =
        jobs::resolveDestination(loaded.value().job, loaded.value().provider, rcloneExecutable);
    if (!resolved) {
        return output.flushError(resolved.error(), kExitFailure);
    }
    jobs::ResolvedDestination destination = std::move(resolved).take();

    Result<QMap<QString, jobs::ManifestSnapshot>> manifest = store.lastSuccessfulManifest(jobId);
    if (!manifest) {
        return output.flushError(manifest.error(), kExitFailure);
    }
    if (manifest.value().isEmpty()) {
        return output.flushError(
            makeErrorValue(ErrorCode::NotFound, "no verified archive entries to check"),
            kExitNotFound);
    }

    int verified = 0;
    int unverifiable = 0;
    int mismatched = 0;
    int missing = 0;
    QStringList problems;

    for (auto it = manifest.value().constBegin(); it != manifest.value().constEnd(); ++it) {
        if (it->sha256.isEmpty()) {
            ++unverifiable;
            continue;
        }
        const Result<QString> remoteHash = destination.provider->checksum(it.key());
        if (!remoteHash) {
            if (remoteHash.error().code() == ErrorCode::NotFound) {
                ++missing;
                problems.append(QStringLiteral("missing: %1").arg(it.key()));
            } else {
                ++unverifiable;
            }
            continue;
        }
        if (remoteHash.value().isEmpty() || remoteHash.value() != it->sha256) {
            ++mismatched;
            problems.append(QStringLiteral("checksum mismatch: %1").arg(it.key()));
            continue;
        }
        ++verified;
    }

    output.add(QStringLiteral("ok"), mismatched == 0 && missing == 0);
    output.add(QStringLiteral("job"), loaded.value().job.name);
    output.add(QStringLiteral("verified"), verified);
    output.add(QStringLiteral("mismatched"), mismatched);
    output.add(QStringLiteral("missing"), missing);
    output.add(QStringLiteral("unverifiable"), unverifiable);
    QJsonArray problemsArray;
    for (const QString& problem : problems) {
        problemsArray.append(problem);
    }
    output.add(QStringLiteral("problems"), problemsArray);
    output.line(QStringLiteral("Verified:     %1").arg(verified));
    output.line(QStringLiteral("Mismatched:   %1").arg(mismatched));
    output.line(QStringLiteral("Missing:      %1").arg(missing));
    output.line(QStringLiteral("Unverifiable: %1 (provider or run recorded no hash)").arg(unverifiable));
    for (const QString& problem : problems) {
        output.line(QStringLiteral("  %1").arg(problem));
    }
    return output.flush(stdout, problems.isEmpty() ? kExitSuccess : kExitFailure);
}

int commandArchiveRestore(Context& context, const Options& options) {
    Output output(options.json);
    if (options.positional.isEmpty()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "archive restore requires a job id"),
            kExitUsage);
    }
    bool valid = false;
    const qint64 jobId = options.positional.constFirst().toLongLong(&valid);
    if (!valid) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "job id must be numeric"), kExitUsage);
    }
    if (!options.flags.contains(QStringLiteral("to"))) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "archive restore requires --to <directory>"),
            kExitUsage);
    }

    jobs::JobStore store(context.database);
    Result<JobContext> loaded = loadJobContext(store, jobId);
    if (!loaded) {
        return output.flushError(loaded.error(),
                                 loaded.error().code() == ErrorCode::NotFound ? kExitNotFound
                                                                              : kExitFailure);
    }

    jobs::ConflictPolicy policy = jobs::ConflictPolicy::Skip;
    if (options.flags.contains(QStringLiteral("overwrite"))) {
        policy = jobs::ConflictPolicy::Overwrite;
    } else if (options.flags.contains(QStringLiteral("rename"))) {
        policy = jobs::ConflictPolicy::Rename;
    } else if (options.flags.contains(QStringLiteral("policy"))) {
        policy = jobs::conflictPolicyFromString(options.flags.value(QStringLiteral("policy")),
                                                jobs::ConflictPolicy::Ask);
        if (policy == jobs::ConflictPolicy::Ask) {
            return output.flushError(
                makeErrorValue(ErrorCode::InvalidArgument,
                               "--policy must be skip, overwrite or rename"),
                kExitUsage);
        }
    }

    config::SettingsStore settings(context.database);
    const QString rcloneExecutable =
        settings.getString(QString::fromUtf8(config::keys::kRcloneBinaryPath), QStringLiteral("rclone"))
            .valueOr(QStringLiteral("rclone"));

    jobs::RestoreRequest request;
    request.job = loaded.value().job;
    request.providerRecord = loaded.value().provider;
    request.destinationDirectory = options.flags.value(QStringLiteral("to"));
    request.policy = policy;
    request.verify = !options.flags.contains(QStringLiteral("no-verify"));

    const CancellationToken cancellation;
    jobs::ArchiveRestorer restorer(context.database, [quiet = options.quiet,
                                                      json = options.json](
                                                         const jobs::RestoreProgress& progress) {
        if (quiet) {
            return;
        }
        QString line = QStringLiteral("[%1/%2]").arg(progress.filesDone).arg(progress.filesTotal);
        if (!progress.currentFile.isEmpty()) {
            line += QStringLiteral(" ") + progress.currentFile;
        }
        if (json) {
            return;
        }
        std::fprintf(stderr, "%s\n", qPrintable(line));
        std::fflush(stderr);
    });
    const jobs::RestoreReport report = restorer.restore(request, cancellation, rcloneExecutable);

    output.add(QStringLiteral("ok"), report.succeeded());
    output.add(QStringLiteral("status"), report.status);
    output.add(QStringLiteral("restore_job_id"), report.restoreJobId);
    output.add(QStringLiteral("restored"), report.restored);
    output.add(QStringLiteral("skipped"), report.skipped);
    output.add(QStringLiteral("renamed"), report.renamed);
    output.add(QStringLiteral("failed"), report.failed);
    output.add(QStringLiteral("verified"), report.verifiedFiles.size());
    output.add(QStringLiteral("bytes_restored"), report.bytesRestored);
    QJsonArray errors;
    for (const QString& error : report.errors) {
        errors.append(error);
    }
    output.add(QStringLiteral("errors"), errors);

    output.line(QStringLiteral("Status:   %1").arg(report.status));
    output.line(QStringLiteral("Restored: %1 file(s), %2 byte(s)").arg(report.restored)
                    .arg(report.bytesRestored));
    output.line(QStringLiteral("Skipped:  %1, renamed: %2, failed: %3")
                    .arg(report.skipped)
                    .arg(report.renamed)
                    .arg(report.failed));
    output.line(QStringLiteral("Verified: %1 file(s)").arg(report.verifiedFiles.size()));
    for (const QString& error : report.errors) {
        output.line(QStringLiteral("  error: %1").arg(error));
    }
    return output.flush(stdout, report.succeeded() ? kExitSuccess : kExitFailure);
}


int commandAgent(Context& context, const Options& options) {
    // The agent talks over its own socket; the database connection is only kept
    // alive so the log file and path overrides stay identical to other commands.
    Q_UNUSED(context);
    Output output(options.json);
    agent::AgentClient client;

    const QString& subcommand = options.subcommand;
    const bool wantsStatus = subcommand.isEmpty() || subcommand == QStringLiteral("status");

    if (wantsStatus) {
        Result<agent::AgentStatus> status = client.status(1500);
        agent::AgentStatus state;
        bool connected = true;
        if (status) {
            state = status.value();
        } else {
            // Fall back to the heartbeat so the answer is still useful when the
            // agent is not running.
            connected = false;
            state = agent::AgentClient::lastKnownStatus();
            state.running = false;
        }

        QJsonObject payload = agent::statusToJson(state);
        payload.insert(QStringLiteral("connected"), connected);
        output.add(QStringLiteral("ok"), connected);
        output.add(QStringLiteral("agent"), payload);
        output.line(QStringLiteral("Agent:       %1").arg(connected ? QStringLiteral("running")
                                                                   : QStringLiteral("not running")));
        if (connected) {
            output.line(QStringLiteral("State:       %1").arg(state.state));
            output.line(QStringLiteral("PID:         %1").arg(state.pid));
            output.line(QStringLiteral("Scheduled:   %1 job(s)").arg(state.scheduledJobs));
            if (!state.lastMessage.isEmpty()) {
                output.line(QStringLiteral("Last:        %1").arg(state.lastMessage));
            }
        } else {
            output.line(QStringLiteral("Last known:  %1 (%2)")
                            .arg(state.state,
                                 state.updatedAtUtc.toString(Qt::ISODate)));
            output.line(QStringLiteral("Start it with: nesva-agent  (or the systemd user service)"));
        }
        return output.flush(stdout, connected ? kExitSuccess : kExitNotFound);
    }

    if (subcommand == QStringLiteral("run") || subcommand == QStringLiteral("cancel")) {
        if (options.positional.isEmpty()) {
            return output.flushError(
                makeErrorValue(ErrorCode::InvalidArgument,
                               QStringLiteral("agent %1 requires a job id")
                                   .arg(subcommand)
                                   .toStdString()),
                kExitUsage);
        }
        bool valid = false;
        const qint64 jobId = options.positional.constFirst().toLongLong(&valid);
        if (!valid) {
            return output.flushError(
                makeErrorValue(ErrorCode::InvalidArgument, "job id must be numeric"), kExitUsage);
        }

        if (subcommand == QStringLiteral("run")) {
            const Result<QString> sent =
                client.runJob(jobId, options.flags.contains(QStringLiteral("dry-run")),
                              QStringLiteral("cli"));
            if (!sent) {
                return output.flushError(sent.error(),
                                         sent.error().code() == ErrorCode::NotFound ? kExitNotFound
                                                                                    : kExitFailure);
            }
            output.add(QStringLiteral("ok"), true);
            output.add(QStringLiteral("accepted"), jobId);
            output.line(QStringLiteral("The agent accepted job %1 and will run it in the background.")
                            .arg(jobId));
            return output.flush(stdout, kExitSuccess);
        }

        const Status cancelled = client.cancelJob(jobId);
        if (!cancelled.ok()) {
            return output.flushError(cancelled.error(), kExitFailure);
        }
        output.add(QStringLiteral("ok"), true);
        output.line(QStringLiteral("Cancellation requested for job %1.").arg(jobId));
        return output.flush(stdout, kExitSuccess);
    }

    if (subcommand == QStringLiteral("reload")) {
        const Status reloaded = client.reload();
        if (!reloaded.ok()) {
            return output.flushError(reloaded.error(), kExitFailure);
        }
        output.add(QStringLiteral("ok"), true);
        output.line(QStringLiteral("Schedules reloaded."));
        return output.flush(stdout, kExitSuccess);
    }

    if (subcommand == QStringLiteral("stop")) {
        const Status stopped = client.shutdown();
        if (!stopped.ok()) {
            return output.flushError(stopped.error(), kExitFailure);
        }
        output.add(QStringLiteral("ok"), true);
        output.line(QStringLiteral("The agent is shutting down gracefully."));
        return output.flush(stdout, kExitSuccess);
    }

    return output.flushError(
        makeErrorValue(ErrorCode::InvalidArgument,
                       QStringLiteral("unknown agent subcommand: %1").arg(subcommand).toStdString()),
        kExitUsage);
}

int commandScheduleList(Context& context, const Options& options) {
    Output output(options.json);
    jobs::JobStore store(context.database);

    Result<QVector<QPair<qint64, jobs::ScheduleRecord>>> schedules = store.listSchedules();
    if (!schedules) {
        return output.flushError(schedules.error(), kExitFailure);
    }

    QList<QStringList> table;
    QJsonArray items;
    for (const auto& entry : schedules.value()) {
        Result<jobs::ArchiveJob> job = store.getJob(entry.first);
        const QString name = job ? job.value().name : QString::number(entry.first);
        const QString nextRun = entry.second.state.nextRunUtc.has_value()
                                    ? entry.second.state.nextRunUtc->toString(Qt::ISODate)
                                    : QStringLiteral("-");

        table.append({QString::number(entry.first), name, sched::describe(entry.second.spec),
                      nextRun, QString::number(entry.second.spec.retryMaxAttempts)});

        QJsonObject item;
        item.insert(QStringLiteral("job_id"), entry.first);
        item.insert(QStringLiteral("job"), name);
        item.insert(QStringLiteral("trigger"), sched::toToken(entry.second.spec.kind));
        item.insert(QStringLiteral("description"), sched::describe(entry.second.spec));
        item.insert(QStringLiteral("next_run_at"), nextRun);
        item.insert(QStringLiteral("retry_max_attempts"), entry.second.spec.retryMaxAttempts);
        item.insert(QStringLiteral("catch_up_missed"), entry.second.spec.catchUpMissed);
        items.append(item);
    }

    output.add(QStringLiteral("ok"), true);
    output.add(QStringLiteral("count"), static_cast<int>(table.size()));
    output.table({QStringLiteral("ID"), QStringLiteral("JOB"), QStringLiteral("SCHEDULE"),
                  QStringLiteral("NEXT RUN (UTC)"), QStringLiteral("RETRIES")},
                 table);
    if (table.isEmpty()) {
        output.line(QStringLiteral("No schedules configured yet."));
    }
    return output.flush(stdout, kExitSuccess);
}


// ---------------------------------------------------------------------------
// Restic repository commands
// ---------------------------------------------------------------------------

jobs::PasswordOptions passwordOptionsFromFlags(const Options& options, Context& context) {
    jobs::PasswordOptions settings;
    config::SettingsStore store(context.database);
    const QString source = store
                               .getString(QString::fromUtf8(config::keys::kBackupPasswordSource),
                                          QStringLiteral("keyring"))
                               .valueOr(QStringLiteral("keyring"));
    settings.source = source == QStringLiteral("file") ? jobs::PasswordSource::File
                                                       : jobs::PasswordSource::Keyring;
    settings.filePath =
        store.getString(QString::fromUtf8(config::keys::kBackupPasswordFile), QString()).valueOr(QString());

    if (options.flags.contains(QStringLiteral("password-file"))) {
        settings.source = jobs::PasswordSource::File;
        settings.filePath = options.flags.value(QStringLiteral("password-file"));
    }
    return settings;
}

/// Reads a password from stdin when --password-stdin is used (no echo handling:
/// the CLI is meant for automation, the GUI prompts in its own dialog).
Result<QByteArray> readPasswordFromStdin(const Options& options) {
    if (!options.flags.contains(QStringLiteral("password-stdin"))) {
        return makeErrorValue(ErrorCode::InvalidArgument, "no password source given");
    }
    std::string line;
    if (!std::getline(std::cin, line)) {
        return makeErrorValue(ErrorCode::InvalidArgument, "no password was read from stdin");
    }
    return QByteArray::fromStdString(line);
}

int commandRepository(Context& context, const Options& options) {
    Output output(options.json);
    const QString& sub = options.subcommand;
    if (options.positional.isEmpty()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument,
                           QStringLiteral("repository %1 requires a job id")
                               .arg(sub.isEmpty() ? QStringLiteral("init") : sub)
                               .toStdString()),
            kExitUsage);
    }
    bool valid = false;
    const qint64 jobId = options.positional.constFirst().toLongLong(&valid);
    if (!valid) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "job id must be numeric"), kExitUsage);
    }

    jobs::JobStore store(context.database);
    Result<jobs::ArchiveJob> job = store.getJob(jobId);
    if (!job) {
        return output.flushError(job.error(), kExitNotFound);
    }
    if (!job.value().isBackup()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "this job is not a Restic backup job"),
            kExitUsage);
    }
    jobs::PasswordOptions passwordOptions = passwordOptionsFromFlags(options, context);

    config::SettingsStore settings(context.database);
    const QString resticExecutable =
        settings.getString(QString::fromUtf8(config::keys::kResticBinaryPath), QStringLiteral("restic"))
            .valueOr(QStringLiteral("restic"));

    const QString& action = sub.isEmpty() ? QStringLiteral("init") : sub;

    if (action == QStringLiteral("init") || action == QStringLiteral("set-password")) {
        Result<QByteArray> password = readPasswordFromStdin(options);
        if (!password && passwordOptions.source == jobs::PasswordSource::File &&
            !passwordOptions.filePath.isEmpty()) {
            QFile file(passwordOptions.filePath);
            if (file.open(QIODevice::ReadOnly)) {
                password = file.readAll().trimmed();
            }
        }
        if (!password) {
            return output.flushError(
                makeErrorValue(ErrorCode::InvalidArgument,
                               "provide the repository password with --password-stdin or "
                               "--password-file"),
                kExitUsage);
        }

        const Status stored =
            jobs::storeRepositoryPassword(job.value().destinationPath, passwordOptions, password.value());
        if (!stored.ok()) {
            return output.flushError(stored.error(), kExitFailure);
        }
        if (action == QStringLiteral("set-password")) {
            output.add(QStringLiteral("ok"), true);
            output.line(QStringLiteral("Repository password stored for '%1'.").arg(job.value().name));
            return output.flush(stdout, kExitSuccess);
        }

        const Result<QString> passwordFile = backup::createPasswordFile(password.value());
        if (!passwordFile) {
            return output.flushError(passwordFile.error(), kExitFailure);
        }
        backup::ResticRepository repository(resticExecutable, job.value().destinationPath,
                                            passwordFile.value());
        const Status initialised = repository.init();
        QFile::remove(passwordFile.value());
        if (!initialised.ok()) {
            return output.flushError(initialised.error(), kExitFailure);
        }
        output.add(QStringLiteral("ok"), true);
        output.add(QStringLiteral("repository"), job.value().destinationPath);
        output.line(QStringLiteral("Repository initialised at %1").arg(job.value().destinationPath));
        return output.flush(stdout, kExitSuccess);
    }

    const Result<QByteArray> password =
        jobs::loadRepositoryPassword(job.value().destinationPath, passwordOptions);
    if (!password) {
        return output.flushError(password.error(), kExitFailure);
    }
    const Result<QString> passwordFile = backup::createPasswordFile(password.value());
    if (!passwordFile) {
        return output.flushError(passwordFile.error(), kExitFailure);
    }
    backup::ResticRepository repository(resticExecutable, job.value().destinationPath,
                                        passwordFile.value());

    if (action == QStringLiteral("locks")) {
        const Result<QVector<backup::LockInfo>> locks = repository.listLocks();
        QFile::remove(passwordFile.value());
        if (!locks) {
            return output.flushError(locks.error(), kExitFailure);
        }
        output.add(QStringLiteral("ok"), true);
        output.add(QStringLiteral("count"), static_cast<int>(locks.value().size()));
        if (locks.value().isEmpty()) {
            output.line(QStringLiteral("No repository locks."));
        } else {
            for (const backup::LockInfo& lock : locks.value()) {
                output.line(QStringLiteral("lock %1").arg(lock.id));
            }
        }
        return output.flush(stdout, kExitSuccess);
    }

    if (action == QStringLiteral("unlock")) {
        const Status unlocked = repository.unlock();
        QFile::remove(passwordFile.value());
        if (!unlocked.ok()) {
            return output.flushError(unlocked.error(), kExitFailure);
        }
        output.add(QStringLiteral("ok"), true);
        output.line(QStringLiteral("Stale locks removed."));
        return output.flush(stdout, kExitSuccess);
    }

    if (action == QStringLiteral("check")) {
        const QString subset = options.flags.value(QStringLiteral("read-data"), QStringLiteral("5%"));
        const Result<backup::VerifyResult> result = repository.check(subset);
        QFile::remove(passwordFile.value());
        if (!result) {
            return output.flushError(result.error(), kExitFailure);
        }
        output.add(QStringLiteral("ok"), result.value().ok);
        output.add(QStringLiteral("errors"), result.value().errors);
        output.line(QStringLiteral("Repository check %1 (%2 error(s))")
                        .arg(result.value().ok ? QStringLiteral("passed") : QStringLiteral("FAILED"))
                        .arg(result.value().errors));
        for (const QString& message : result.value().messages) {
            output.line(QStringLiteral("  %1").arg(message));
        }
        return output.flush(stdout, result.value().ok ? kExitSuccess : kExitFailure);
    }

    QFile::remove(passwordFile.value());
    return output.flushError(
        makeErrorValue(ErrorCode::InvalidArgument,
                       QStringLiteral("unknown repository command: %1").arg(action).toStdString()),
        kExitUsage);
}

int commandSnapshotsList(Context& context, const Options& options) {
    Output output(options.json);
    if (options.positional.isEmpty()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "snapshots list requires a job id"),
            kExitUsage);
    }
    bool valid = false;
    const qint64 jobId = options.positional.constFirst().toLongLong(&valid);
    if (!valid) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "job id must be numeric"), kExitUsage);
    }

    jobs::JobStore store(context.database);
    Result<jobs::ArchiveJob> job = store.getJob(jobId);
    if (!job) {
        return output.flushError(job.error(), kExitNotFound);
    }
    if (!job.value().isBackup()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "this job is not a Restic backup job"),
            kExitUsage);
    }

    jobs::PasswordOptions passwordOptions = passwordOptionsFromFlags(options, context);
    config::SettingsStore settings(context.database);
    const QString resticExecutable =
        settings.getString(QString::fromUtf8(config::keys::kResticBinaryPath), QStringLiteral("restic"))
            .valueOr(QStringLiteral("restic"));
    const Result<QByteArray> password =
        jobs::loadRepositoryPassword(job.value().destinationPath, passwordOptions);
    if (!password) {
        return output.flushError(password.error(), kExitFailure);
    }
    const Result<QString> passwordFile = backup::createPasswordFile(password.value());
    if (!passwordFile) {
        return output.flushError(passwordFile.error(), kExitFailure);
    }
    backup::ResticRepository repository(resticExecutable, job.value().destinationPath,
                                        passwordFile.value());
    const Result<QVector<backup::SnapshotInfo>> snapshots = repository.snapshots();
    QFile::remove(passwordFile.value());
    if (!snapshots) {
        return output.flushError(snapshots.error(), kExitFailure);
    }

    QList<QStringList> table;
    QJsonArray items;
    for (const backup::SnapshotInfo& snapshot : snapshots.value()) {
        table.append({snapshot.shortId, snapshot.time.toLocalTime().toString(Qt::ISODate),
                      snapshot.hostname, snapshot.paths.join(QStringLiteral(", ")),
                      snapshot.totalBytes >= 0 ? QString::number(snapshot.totalBytes)
                                               : QStringLiteral("-")});
        QJsonObject item;
        item.insert(QStringLiteral("id"), snapshot.id);
        item.insert(QStringLiteral("short_id"), snapshot.shortId);
        item.insert(QStringLiteral("time"), snapshot.time.toUTC().toString(Qt::ISODate));
        item.insert(QStringLiteral("hostname"), snapshot.hostname);
        item.insert(QStringLiteral("bytes"), snapshot.totalBytes);
        items.append(item);
    }
    output.add(QStringLiteral("ok"), true);
    output.add(QStringLiteral("job"), job.value().name);
    output.add(QStringLiteral("count"), static_cast<int>(table.size()));
    output.add(QStringLiteral("snapshots"), items);
    output.table({QStringLiteral("ID"), QStringLiteral("TIME"), QStringLiteral("HOST"),
                  QStringLiteral("PATHS"), QStringLiteral("BYTES")},
                 table);
    if (table.isEmpty()) {
        output.line(QStringLiteral("No snapshots yet."));
    }
    return output.flush(stdout, kExitSuccess);
}

int commandRetention(Context& context, const Options& options) {
    Output output(options.json);
    if (options.positional.isEmpty()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "retention requires a job id"), kExitUsage);
    }
    bool valid = false;
    const qint64 jobId = options.positional.constFirst().toLongLong(&valid);
    if (!valid) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "job id must be numeric"), kExitUsage);
    }

    jobs::JobStore store(context.database);
    Result<jobs::ArchiveJob> job = store.getJob(jobId);
    if (!job) {
        return output.flushError(job.error(), kExitNotFound);
    }
    if (!job.value().isBackup()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "retention applies to Restic backup jobs only"),
            kExitUsage);
    }

    const bool apply = options.subcommand == QStringLiteral("apply");
    const bool prune = options.flags.contains(QStringLiteral("prune"));
    jobs::PasswordOptions passwordOptions = passwordOptionsFromFlags(options, context);
    config::SettingsStore settings(context.database);
    const QString resticExecutable =
        settings.getString(QString::fromUtf8(config::keys::kResticBinaryPath), QStringLiteral("restic"))
            .valueOr(QStringLiteral("restic"));

    const Result<QByteArray> password =
        jobs::loadRepositoryPassword(job.value().destinationPath, passwordOptions);
    if (!password) {
        return output.flushError(password.error(), kExitFailure);
    }
    const Result<QString> passwordFile = backup::createPasswordFile(password.value());
    if (!passwordFile) {
        return output.flushError(passwordFile.error(), kExitFailure);
    }
    backup::ResticRepository repository(resticExecutable, job.value().destinationPath,
                                        passwordFile.value());
    const Result<QVector<backup::ForgetGroup>> groups =
        repository.forget(job.value().retention, !apply, prune);
    QFile::remove(passwordFile.value());
    if (!groups) {
        return output.flushError(groups.error(), kExitFailure);
    }

    int kept = 0;
    int removed = 0;
    QStringList removedIds;
    for (const backup::ForgetGroup& group : groups.value()) {
        kept += group.keptIds.size();
        removed += group.removedIds.size();
        removedIds.append(group.removedIds);
    }

    output.add(QStringLiteral("ok"), true);
    output.add(QStringLiteral("dry_run"), !apply);
    output.add(QStringLiteral("kept"), kept);
    output.add(QStringLiteral("removed"), removed);
    output.add(QStringLiteral("prune"), apply && prune);
    output.line(QStringLiteral("%1: keep %2 snapshot(s), %3 %4")
                    .arg(apply ? QStringLiteral("Retention applied") : QStringLiteral("Retention preview"))
                    .arg(kept)
                    .arg(apply ? QStringLiteral("removed") : QStringLiteral("would remove"))
                    .arg(removed));
    for (const QString& id : removedIds) {
        output.line(QStringLiteral("  %1").arg(id));
    }
    if (!apply) {
        output.line(QStringLiteral("Nothing was deleted. Re-run with 'retention apply' to execute."));
    }
    return output.flush(stdout, kExitSuccess);
}

int commandBackupRestore(Context& context, const Options& options) {
    Output output(options.json);
    if (options.positional.isEmpty()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "backup restore requires a job id"),
            kExitUsage);
    }
    bool valid = false;
    const qint64 jobId = options.positional.constFirst().toLongLong(&valid);
    if (!valid) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "job id must be numeric"), kExitUsage);
    }
    if (!options.flags.contains(QStringLiteral("to"))) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "backup restore requires --to <directory>"),
            kExitUsage);
    }

    jobs::JobStore store(context.database);
    Result<jobs::ArchiveJob> job = store.getJob(jobId);
    if (!job) {
        return output.flushError(job.error(), kExitNotFound);
    }
    if (!job.value().isBackup()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "this job is not a Restic backup job"),
            kExitUsage);
    }

    jobs::BackupRestorer::Request request;
    request.job = job.value();
    request.snapshotId = options.flags.value(QStringLiteral("snapshot"), QStringLiteral("latest"));
    request.destinationDirectory = options.flags.value(QStringLiteral("to"));
    request.policy = jobs::conflictPolicyFromString(
        options.flags.value(QStringLiteral("policy"),
                            options.flags.contains(QStringLiteral("overwrite"))
                                ? QStringLiteral("overwrite")
                                : (options.flags.contains(QStringLiteral("rename"))
                                       ? QStringLiteral("rename")
                                       : QStringLiteral("skip"))),
        jobs::ConflictPolicy::Ask);
    if (request.policy == jobs::ConflictPolicy::Ask) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument,
                           "--policy must be skip, overwrite or rename"),
            kExitUsage);
    }
    request.passwordOptions = passwordOptionsFromFlags(options, context);
    config::SettingsStore settings(context.database);
    request.resticExecutable =
        settings.getString(QString::fromUtf8(config::keys::kResticBinaryPath), QStringLiteral("restic"))
            .valueOr(QStringLiteral("restic"));

    const CancellationToken cancellation;
    jobs::BackupRestorer restorer(context.database);
    const jobs::BackupRestorer::Report report = restorer.restore(request, cancellation);

    output.add(QStringLiteral("ok"), report.succeeded());
    output.add(QStringLiteral("status"), report.status);
    output.add(QStringLiteral("restored"), report.restored);
    output.add(QStringLiteral("skipped"), report.skipped);
    output.add(QStringLiteral("renamed"), report.renamed);
    output.add(QStringLiteral("failed"), report.failed);
    output.add(QStringLiteral("bytes"), report.bytesRestored);
    QJsonArray errors;
    for (const QString& error : report.errors) {
        errors.append(error);
    }
    output.add(QStringLiteral("errors"), errors);

    output.line(QStringLiteral("Status:   %1").arg(report.status));
    output.line(QStringLiteral("Restored: %1 file(s), %2 byte(s)").arg(report.restored)
                    .arg(report.bytesRestored));
    output.line(QStringLiteral("Skipped:  %1, renamed: %2, failed: %3")
                    .arg(report.skipped)
                    .arg(report.renamed)
                    .arg(report.failed));
    for (const QString& error : report.errors) {
        output.line(QStringLiteral("  error: %1").arg(error));
    }
    return output.flush(stdout, report.succeeded() ? kExitSuccess : kExitFailure);
}

} // namespace

int run(int argc, char** argv) {
    QStringList arguments;
    arguments.reserve(argc > 0 ? argc - 1 : 0);
    for (int index = 1; index < argc; ++index) {
        arguments.append(QString::fromLocal8Bit(argv[index]));
    }

    const Options options = parseArguments(arguments);

    if (options.versionRequested || options.command == QStringLiteral("version")) {
        const QByteArray text = versionText().toUtf8() + '\n';
        std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stdout);
        return kExitSuccess;
    }

    if (options.helpRequested || options.command == QStringLiteral("help")) {
        const QByteArray text = usageText().toUtf8();
        std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stdout);
        return kExitSuccess;
    }

    if (options.command.isEmpty()) {
        const QByteArray text = usageText().toUtf8();
        std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stderr);
        return kExitUsage;
    }

    if (!options.parseErrors.isEmpty()) {
        const QByteArray text = (QStringLiteral("error: %1\n\n").arg(options.parseErrors.first()) +
                                 usageText())
                                    .toUtf8();
        std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stderr);
        return kExitUsage;
    }

    Context context;
    const Result<QString> prepared = prepareContext(context, options);
    if (!prepared) {
        Output output(options.json);
        const int code = exitCodeFor(prepared.error().code());
        return output.flushError(prepared.error(), code == kExitSuccess ? kExitFailure : code);
    }
    context.logFile = prepared.value();

    const QString& command = options.command;
    const QString& subcommand = options.subcommand;

    if (command == QStringLiteral("status")) {
        return commandStatus(context, options);
    }
    if (command == QStringLiteral("doctor")) {
        return commandDoctor(context, options);
    }
    if (command == QStringLiteral("history")) {
        if (subcommand.isEmpty() || subcommand == QStringLiteral("list")) {
            return commandHistoryList(context, options);
        }
        return commandNotImplemented(options, QStringLiteral("history %1").arg(subcommand),
                                     QStringLiteral("a later phase"));
    }
    if (command == QStringLiteral("storage")) {
        if (subcommand.isEmpty() || subcommand == QStringLiteral("list")) {
            return commandStorageList(context, options);
        }
        if (subcommand == QStringLiteral("test")) {
            return commandStorageTest(context, options);
        }
        return commandNotImplemented(options, QStringLiteral("storage %1").arg(subcommand),
                                     QStringLiteral("a later phase"));
    }
    if (command == QStringLiteral("jobs")) {
        if (subcommand.isEmpty() || subcommand == QStringLiteral("list")) {
            return commandJobsList(context, options);
        }
        if (subcommand == QStringLiteral("run")) {
            return commandJobsRun(context, options);
        }
        if (subcommand == QStringLiteral("cancel")) {
            return commandJobsCancel(context, options);
        }
        return commandNotImplemented(options, QStringLiteral("jobs %1").arg(subcommand),
                                     QStringLiteral("a later phase"));
    }
    if (command == QStringLiteral("repository")) {
        return commandRepository(context, options);
    }
    if (command == QStringLiteral("snapshots")) {
        return commandSnapshotsList(context, options);
    }
    if (command == QStringLiteral("retention")) {
        return commandRetention(context, options);
    }
    if (command == QStringLiteral("backup")) {
        if (subcommand == QStringLiteral("restore")) {
            return commandBackupRestore(context, options);
        }
        return commandNotImplemented(options, QStringLiteral("backup %1").arg(subcommand),
                                     QStringLiteral("a later phase"));
    }
    if (command == QStringLiteral("agent")) {
        return commandAgent(context, options);
    }
    if (command == QStringLiteral("schedule")) {
        if (subcommand.isEmpty() || subcommand == QStringLiteral("list")) {
            return commandScheduleList(context, options);
        }
        return commandNotImplemented(options, QStringLiteral("schedule %1").arg(subcommand),
                                     QStringLiteral("the schedule editor is in the desktop app"));
    }
    if (command == QStringLiteral("archive")) {
        if (subcommand.isEmpty() || subcommand == QStringLiteral("list")) {
            return commandArchiveList(context, options);
        }
        if (subcommand == QStringLiteral("restore")) {
            return commandArchiveRestore(context, options);
        }
        return commandNotImplemented(options, QStringLiteral("archive %1").arg(subcommand),
                                     QStringLiteral("a later phase"));
    }
    if (command == QStringLiteral("verify")) {
        return commandVerify(context, options);
    }

    Output output(options.json);
    return output.flushError(
        makeErrorValue(ErrorCode::InvalidArgument,
                       QStringLiteral("unknown command: %1").arg(command).toStdString()),
        kExitUsage);
}

} // namespace nesva::cli
