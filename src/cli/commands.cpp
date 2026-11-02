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
#include "nesva/jobs/job_store.hpp"
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
    jobs::ArchiveRunner runner(context.database, [quiet = options.quiet, json = options.json](
                                                       const jobs::RunProgress& progress) {
        if (!quiet) {
            printProgressLine(progress, json);
        }
    });

    const jobs::RunReport report = runner.run(loaded.value().job, loaded.value().provider, runOptions,
                                              cancellation, QStringLiteral("cli"), rcloneExecutable);

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
