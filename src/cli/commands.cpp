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
#include "nesva/mail/exporter.hpp"
#include "nesva/mail/graph.hpp"
#include "nesva/mail/mail_store.hpp"
#include "nesva/security/secret_store.hpp"
#include "nesva/jobs/backup_runner.hpp"
#include "nesva/jobs/job_store.hpp"
#include "nesva/scheduler/schedule.hpp"
#include "nesva/providers/provider_factory.hpp"
#include "nesva/tools/tool_installer.hpp"

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

/// Durations as a person reads them: "45s", "3m 12s", "1h 05m", "2d 04h".
QString humanDuration(qint64 seconds) {
    if (seconds < 0) {
        return QStringLiteral("unknown");
    }
    if (seconds < 60) {
        return QStringLiteral("%1s").arg(seconds);
    }
    if (seconds < 3600) {
        return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    }
    if (seconds < 86400) {
        const qint64 minutes = (seconds % 3600) / 60;
        return QStringLiteral("%1h %2m").arg(seconds / 3600).arg(minutes, 2, 10, QLatin1Char('0'));
    }
    const qint64 hours = (seconds % 86400) / 3600;
    return QStringLiteral("%1d %2h").arg(seconds / 86400).arg(hours, 2, 10, QLatin1Char('0'));
}

/// Byte counts in binary units, one decimal above a kilobyte.
QString humanBytes(qint64 bytes) {
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    const QStringList units = {QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB"),
                               QStringLiteral("TB")};
    double value = static_cast<double>(bytes) / 1024.0;
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(units.at(unit));
}

/// One-line sync progress. Written with \r so a terminal shows a single
/// updating line; the padding clears the remains of a longer previous line.
void renderMailProgress(const QString& text) {
    std::fprintf(stderr, "\r%-110s", qPrintable(text));
    std::fflush(stderr);
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


/// Verifies that restic is installed and new enough before a repository command.
bool ensureRestic(Output& output, const QString& executable) {
    const Status ready = backup::requireResticVersion(executable);
    if (ready.ok()) {
        return true;
    }
    output.flushError(ready.error(), kExitFailure);
    return false;
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

int commandTools(Context& context, const Options& options) {
    Output output(options.json);
    config::SettingsStore settings(context.database);
    const QString& sub = options.subcommand;

    const auto settingOrDefault = [&settings](const char* key, const QString& fallback) {
        const Result<QString> value = settings.getString(QString::fromUtf8(key), fallback);
        return value && !value.value().trimmed().isEmpty() ? value.value() : fallback;
    };

    if (sub.isEmpty() || sub == QStringLiteral("status")) {
        QList<QStringList> table;
        QJsonArray toolsJson;
        struct ToolRow {
            const char* key;
            QString name;
        };
        const QList<ToolRow> rows = {
            {config::keys::kRcloneBinaryPath, QStringLiteral("rclone")},
            {config::keys::kResticBinaryPath, QStringLiteral("restic")},
        };
        for (const ToolRow& row : rows) {
            const QString configured = settingOrDefault(row.key, row.name);
            const Result<ExternalToolInfo> probe =
                probeExternalTool(configured, {QStringLiteral("version")}, 8000);
            QJsonObject entry;
            entry.insert(QStringLiteral("tool"), row.name);
            entry.insert(QStringLiteral("configured"), configured);
            if (probe) {
                entry.insert(QStringLiteral("path"), probe.value().executable);
                entry.insert(QStringLiteral("version"), probe.value().versionText);
                entry.insert(QStringLiteral("found"), true);
                table.append({row.name, probe.value().executable, probe.value().versionText});
            } else {
                entry.insert(QStringLiteral("found"), false);
                table.append({row.name, QStringLiteral("-"),
                              QStringLiteral("not installed (nesva-cli tools %1 --install)")
                                  .arg(row.name)});
            }
            toolsJson.append(entry);
        }
        output.add(QStringLiteral("ok"), true);
        output.add(QStringLiteral("items"), toolsJson);
        output.table({QStringLiteral("TOOL"), QStringLiteral("PATH"), QStringLiteral("VERSION")},
                     table);
        return output.flush(stdout, kExitSuccess);
    }

    if (sub == QStringLiteral("rclone")) {
        const bool install = options.flags.contains(QStringLiteral("install"));
        const tools::InstallScope scope =
            options.flags.contains(QStringLiteral("system")) ? tools::InstallScope::System
                                                             : tools::InstallScope::User;

        net::QNetworkHttpTransport transport;
        const Result<tools::ToolInstallPlan> plan = tools::planRcloneInstall(scope, transport);
        if (!plan) {
            return output.flushError(plan.error(), kExitFailure);
        }
        const tools::ToolInstallPlan& value = plan.value();

        output.add(QStringLiteral("ok"), true);
        output.add(QStringLiteral("version"), value.version);
        output.add(QStringLiteral("download_url"), value.downloadUrl);
        output.add(QStringLiteral("checksum_url"), value.checksumUrl);
        output.add(QStringLiteral("target"), value.targetPath);
        output.add(QStringLiteral("scope"), tools::toToken(value.scope));
        output.add(QStringLiteral("requires_elevation"), value.requiresElevation);
        output.add(QStringLiteral("automatic"), value.automatic);
        output.add(QStringLiteral("installed"), false);

        if (!install) {
            output.line(QStringLiteral("Installation plan - nothing has been downloaded, written "
                                       "or executed yet:"));
            for (const QString& line :
                 tools::describeInstallPlan(value).split(QLatin1Char('\n'))) {
                output.line(QStringLiteral("  %1").arg(line));
            }
            if (!value.automatic) {
                output.line(QStringLiteral("This installation cannot be performed automatically on "
                                           "this system: run the commands shown above."));
                return output.flush(stdout, kExitSuccess);
            }
            output.line(QStringLiteral("Run '%1' to perform exactly these steps.")
                            .arg(options.flags.contains(QStringLiteral("system"))
                                     ? QStringLiteral("nesva-cli tools rclone --install --system")
                                     : QStringLiteral("nesva-cli tools rclone --install")));
            return output.flush(stdout, kExitSuccess);
        }

        const auto progress = [quiet = options.quiet](const QString& text) {
            if (quiet) {
                return;
            }
            std::fprintf(stderr, "  %s\n", qPrintable(text));
            std::fflush(stderr);
        };
        const Result<tools::InstallOutcome> outcome =
            tools::executeRcloneInstall(value, progress);
        if (!outcome) {
            return output.flushError(outcome.error(), kExitFailure);
        }

        // Record the absolute path: Nesva then uses this binary even when the
        // install directory is not on PATH (the usual case for ~/.local/bin).
        const Status stored = settings.setString(
            QString::fromUtf8(config::keys::kRcloneBinaryPath), outcome.value().executablePath);
        if (!stored.ok()) {
            return output.flushError(stored.error(), kExitFailure);
        }

        output.add(QStringLiteral("installed"), true);
        output.add(QStringLiteral("installed_version"), outcome.value().version);
        output.add(QStringLiteral("installed_path"), outcome.value().executablePath);
        output.add(QStringLiteral("sha256"), outcome.value().sha256);
        output.add(QStringLiteral("bytes_downloaded"), outcome.value().bytesDownloaded);
        output.line(QStringLiteral("Installed rclone %1 at %2 (SHA-256 verified).")
                        .arg(outcome.value().version, outcome.value().executablePath));
        output.line(QStringLiteral("Nesva now uses that path, so it works without changing PATH."));
        output.line(QStringLiteral("To also use rclone in a terminal, add %1 to PATH.")
                        .arg(QFileInfo(outcome.value().executablePath).absolutePath()));
        return output.flush(stdout, kExitSuccess);
    }

    return output.flushError(
        makeErrorValue(ErrorCode::InvalidArgument,
                       QStringLiteral("unknown tools command: %1").arg(sub).toStdString()),
        kExitUsage);
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
        if (!ensureRestic(output, resticExecutable)) {
            QFile::remove(passwordFile.value());
            return kExitFailure;
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


    if (action == QStringLiteral("info")) {
        const QString account = jobs::repositoryAccount(job.value().destinationPath);
        QString stored = QStringLiteral("not stored");
        QString backend = QStringLiteral("keyring");
        if (passwordOptions.source == jobs::PasswordSource::File) {
            backend = QStringLiteral("file");
            stored = QFileInfo::exists(passwordOptions.filePath) ? QStringLiteral("present")
                                                                 : QStringLiteral("missing");
        } else {
            const Result<QByteArray> lookup =
                jobs::loadRepositoryPassword(job.value().destinationPath, passwordOptions);
            stored = lookup ? QStringLiteral("present in the keyring")
                            : QStringLiteral("not found (%1)")
                                  .arg(QString::fromStdString(lookup.error().message()));
        }

        output.add(QStringLiteral("ok"), true);
        output.add(QStringLiteral("job"), job.value().name);
        output.add(QStringLiteral("repository"), job.value().destinationPath);
        output.add(QStringLiteral("password_backend"), backend);
        output.add(QStringLiteral("keyring_service"), passwordOptions.keyringService);
        output.add(QStringLiteral("keyring_account"), account);
        if (backend == QStringLiteral("file")) {
            output.add(QStringLiteral("password_file"), passwordOptions.filePath);
        }
        output.add(QStringLiteral("password_state"), stored);

        output.line(QStringLiteral("Job:        %1").arg(job.value().name));
        output.line(QStringLiteral("Repository: %1").arg(job.value().destinationPath));
        output.line(QStringLiteral("Backend:    %1").arg(backend));
        if (backend == QStringLiteral("file")) {
            output.line(QStringLiteral("Password:   %1 (%2)").arg(passwordOptions.filePath, stored));
        } else {
            output.line(QStringLiteral("Keyring:    service=%1 account=%2")
                            .arg(passwordOptions.keyringService, account));
            output.line(QStringLiteral("Password:   %1").arg(stored));
        }
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
    if (!ensureRestic(output, resticExecutable)) {
        QFile::remove(passwordFile.value());
        return kExitFailure;
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
    if (!ensureRestic(output, resticExecutable)) {
        QFile::remove(passwordFile.value());
        return kExitFailure;
    }
    if (!ensureRestic(output, resticExecutable)) {
        QFile::remove(passwordFile.value());
        return kExitFailure;
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
    if (!ensureRestic(output, resticExecutable)) {
        QFile::remove(passwordFile.value());
        return kExitFailure;
    }
    if (!ensureRestic(output, resticExecutable)) {
        QFile::remove(passwordFile.value());
        return kExitFailure;
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


// ---------------------------------------------------------------------------
// Microsoft 365 mail archival
// ---------------------------------------------------------------------------

struct MailConfig {
    QString tenantId;
    QString clientId;
    QString destination;
};

MailConfig mailConfigFromSettings(Context& context, const Options& options) {
    config::SettingsStore settings(context.database);
    MailConfig configuration;
    configuration.tenantId =
        settings.getString(QStringLiteral("mail.tenant_id"), QString()).valueOr(QString());
    configuration.clientId =
        settings.getString(QStringLiteral("mail.client_id"), QString()).valueOr(QString());
    configuration.destination =
        settings.getString(QStringLiteral("mail.destination"), QString()).valueOr(QString());
    if (options.flags.contains(QStringLiteral("tenant"))) {
        configuration.tenantId = options.flags.value(QStringLiteral("tenant"));
    }
    if (options.flags.contains(QStringLiteral("client"))) {
        configuration.clientId = options.flags.value(QStringLiteral("client"));
    }
    if (options.flags.contains(QStringLiteral("to"))) {
        configuration.destination = options.flags.value(QStringLiteral("to"));
    }
    return configuration;
}

QString mailTokenAccount(const QString& tenantId, const QString& upn) {
    return QStringLiteral("msgraph/%1/%2").arg(tenantId, upn);
}

Result<QByteArray> loadMailToken(const QString& tenantId, const QString& upn) {
    std::unique_ptr<security::SecretStore> secrets = security::createSecretStore();
    if (!secrets->isAvailable()) {
        return makeErrorValue(ErrorCode::Unsupported, "no credential store is available",
                              secrets->unavailableReason().toStdString());
    }
    return secrets->lookup(QStringLiteral("nesva"), mailTokenAccount(tenantId, upn));
}

Status storeMailToken(const QString& tenantId, const QString& upn, const QString& token) {
    std::unique_ptr<security::SecretStore> secrets = security::createSecretStore();
    if (!secrets->isAvailable()) {
        return makeError(ErrorCode::Unsupported, "no credential store is available",
                         secrets->unavailableReason().toStdString());
    }
    return secrets->store(QStringLiteral("nesva"), mailTokenAccount(tenantId, upn),
                          token.toUtf8());
}

/// Builds an authenticated Graph client for a stored account.
Result<std::shared_ptr<mail::GraphClient>> openGraph(Context& context, const mail::MailAccount& account) {
    Q_UNUSED(context);
    const Result<QByteArray> token = loadMailToken(account.tenantId, account.accountIdentifier);
    if (!token) {
        return makeErrorValue(ErrorCode::PermissionDenied,
                              "no Microsoft 365 refresh token is stored for this account",
                              QString::fromStdString(token.error().message()).toStdString());
    }
    mail::OAuthConfig oauth;
    oauth.tenantId = account.tenantId;
    oauth.clientId = account.clientId;
    auto transport = std::make_shared<net::QNetworkHttpTransport>();
    auto provider = std::make_shared<mail::TokenProvider>(
        oauth, QString::fromUtf8(token.value()), transport);
    return std::make_shared<mail::GraphClient>(provider, transport);
}

int commandMail(Context& context, const Options& options) {
    Output output(options.json);
    const QString& sub = options.subcommand;
    const MailConfig configuration = mailConfigFromSettings(context, options);

    if (sub.isEmpty() || sub == QStringLiteral("help")) {
        output.line(QStringLiteral("mail connect   --account <upn> [--tenant <id>] [--client <id>]"));
        output.line(QStringLiteral("mail accounts"));
        output.line(QStringLiteral("mail folders <account-id> [--enable <remote-id>] [--disable <remote-id>]"));
        output.line(QStringLiteral("mail estimate <account-id>"));
        output.line(QStringLiteral("mail sync <account-id> --to <directory> [--full] [--max-messages N]"));
        output.line(QStringLiteral("mail status <account-id>"));
        return output.flush(stdout, kExitSuccess);
    }

    if (sub == QStringLiteral("connect")) {
        const QString upn = options.flags.value(QStringLiteral("account"));
        if (upn.trimmed().isEmpty() || configuration.tenantId.isEmpty() ||
            configuration.clientId.isEmpty()) {
            return output.flushError(
                makeErrorValue(ErrorCode::InvalidArgument,
                               QStringLiteral("usage: mail connect --account <upn> "
                                              "[--tenant <tenant-id>] [--client <client-id>]")
                                   .toStdString()),
                kExitUsage);
        }

        mail::OAuthConfig oauth;
        oauth.tenantId = configuration.tenantId;
        oauth.clientId = configuration.clientId;

        const Result<mail::TokenSet> tokens = mail::interactiveAuthorize(
            oauth, std::make_shared<net::QNetworkHttpTransport>(), 300000,
            [](const QString& url) {
                // Print immediately: the user has to act on this while the
                // command waits for the browser redirect.
                std::fprintf(stderr, "\nOpen this URL in a browser to sign in:\n%s\n\n", qPrintable(url));
                std::fprintf(stderr, "Waiting for the sign-in to complete...\n");
                std::fflush(stderr);
            });
        if (!tokens) {
            std::fprintf(stderr,
                         "\nAn authorization code can be redeemed only once; "
                         "run this command again to start a fresh sign-in.\n");
            std::fflush(stderr);
            return output.flushError(tokens.error(), kExitFailure);
        }
        if (tokens.value().refreshToken.isEmpty()) {
            return output.flushError(
                makeErrorValue(ErrorCode::PermissionDenied,
                               "Microsoft did not return a refresh token; the offline_access "
                               "scope must be granted"),
                kExitFailure);
        }

        const Status stored =
            storeMailToken(configuration.tenantId, upn, tokens.value().refreshToken);
        if (!stored.ok()) {
            return output.flushError(stored.error(), kExitFailure);
        }

        mail::OAuthConfig verifyConfig = oauth;
        auto transport = std::make_shared<net::QNetworkHttpTransport>();
        auto provider = std::make_shared<mail::TokenProvider>(
            verifyConfig, tokens.value().refreshToken, transport);
        provider->setRefreshToken(tokens.value().refreshToken);
        auto client = std::make_shared<mail::GraphClient>(provider, transport);

        const Result<QJsonObject> me = client->me();
        const QString resolvedUpn = me
                                        ? me.value().value(QStringLiteral("userPrincipalName")).toString()
                                        : upn;
        mail::MailStore store(context.database);
        mail::MailAccount account;
        account.displayName = me ? me.value().value(QStringLiteral("displayName")).toString()
                                 : upn;
        if (account.displayName.isEmpty()) {
            account.displayName = upn;
        }
        account.tenantId = configuration.tenantId;
        account.clientId = configuration.clientId;
        account.accountIdentifier = resolvedUpn;
        account.scopes = oauth.scopes.join(QLatin1Char(' '));
        account.status = QStringLiteral("connected");
        const Result<qint64> accountId = store.upsertAccount(account);
        if (!accountId) {
            return output.flushError(accountId.error(), kExitFailure);
        }

        const Result<QVector<mail::MailFolderInfo>> folders = client->mailFolders();
        if (!folders) {
            return output.flushError(folders.error(), kExitFailure);
        }
        const Status stored2 = store.storeFolders(accountId.value(), folders.value());
        if (!stored2.ok()) {
            return output.flushError(stored2.error(), kExitFailure);
        }

        output.add(QStringLiteral("ok"), true);
        output.add(QStringLiteral("account_id"), accountId.value());
        output.add(QStringLiteral("account"), resolvedUpn);
        output.add(QStringLiteral("folders"), static_cast<int>(folders.value().size()));
        output.line(QStringLiteral("Connected %1 (account id %2, %3 folders discovered).")
                        .arg(resolvedUpn)
                        .arg(accountId.value())
                        .arg(folders.value().size()));
        output.line(QStringLiteral("The refresh token is stored in the system keyring, never in the database."));
        return output.flush(stdout, kExitSuccess);
    }

    if (sub == QStringLiteral("accounts")) {
        mail::MailStore store(context.database);
        const Result<QVector<mail::MailAccount>> accounts = store.accounts();
        if (!accounts) {
            return output.flushError(accounts.error(), kExitFailure);
        }
        QList<QStringList> table;
        for (const mail::MailAccount& account : accounts.value()) {
            const Result<QVector<mail::MailFolderRecord>> folders = store.folders(account.id);
            const Result<QVector<mail::MailMessageRecord>> messages = store.messages(account.id, 1);
            table.append({QString::number(account.id), account.accountIdentifier,
                          account.displayName, account.status,
                          QString::number(folders ? folders.value().size() : 0),
                          messages ? QStringLiteral("yes") : QStringLiteral("no")});
        }
        output.add(QStringLiteral("ok"), true);
        output.add(QStringLiteral("count"), static_cast<int>(table.size()));
        output.table({QStringLiteral("ID"), QStringLiteral("ACCOUNT"), QStringLiteral("NAME"),
                      QStringLiteral("STATUS"), QStringLiteral("FOLDERS"),
                      QStringLiteral("HAS MAIL")},
                     table);
        if (table.isEmpty()) {
            output.line(QStringLiteral("No Microsoft 365 accounts are connected yet."));
        }
        return output.flush(stdout, kExitSuccess);
    }

    if (options.positional.isEmpty()) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument,
                           QStringLiteral("mail %1 requires an account id").arg(sub).toStdString()),
            kExitUsage);
    }
    bool valid = false;
    const qint64 accountId = options.positional.constFirst().toLongLong(&valid);
    if (!valid) {
        return output.flushError(
            makeErrorValue(ErrorCode::InvalidArgument, "account id must be numeric"), kExitUsage);
    }

    mail::MailStore store(context.database);
    const Result<mail::MailAccount> account = store.account(accountId);
    if (!account) {
        return output.flushError(account.error(), kExitNotFound);
    }

    if (sub == QStringLiteral("status")) {
        const Result<QVector<mail::MailMessageRecord>> messages = store.messages(accountId, 100000);
        if (!messages) {
            return output.flushError(messages.error(), kExitFailure);
        }
        int exported = 0;
        int pending = 0;
        int failed = 0;
        for (const mail::MailMessageRecord& message : messages.value()) {
            if (message.exportState == QStringLiteral("exported")) {
                ++exported;
            } else if (message.exportState == QStringLiteral("failed")) {
                ++failed;
            } else {
                ++pending;
            }
        }
        output.add(QStringLiteral("ok"), true);
        output.add(QStringLiteral("account"), account.value().accountIdentifier);
        output.add(QStringLiteral("exported"), exported);
        output.add(QStringLiteral("pending"), pending);
        output.add(QStringLiteral("failed"), failed);
        output.line(QStringLiteral("Account:  %1 (%2)").arg(account.value().accountIdentifier,
                                                           account.value().status));
        output.line(QStringLiteral("Messages: %1 exported, %2 pending, %3 failed")
                        .arg(exported)
                        .arg(pending)
                        .arg(failed));
        return output.flush(stdout, kExitSuccess);
    }

    if (sub == QStringLiteral("folders")) {
        Result<QVector<mail::MailFolderRecord>> folders = store.folders(accountId);
        if (!folders) {
            return output.flushError(folders.error(), kExitFailure);
        }
        if (options.flags.contains(QStringLiteral("enable")) ||
            options.flags.contains(QStringLiteral("disable"))) {
            const bool enable = options.flags.contains(QStringLiteral("enable"));
            const QString remoteId = options.flags.value(
                enable ? QStringLiteral("enable") : QStringLiteral("disable"));
            bool found = false;
            for (const mail::MailFolderRecord& folder : folders.value()) {
                if (folder.remoteId == remoteId) {
                    const Status status = store.setFolderSyncEnabled(folder.id, enable);
                    if (!status.ok()) {
                        return output.flushError(status.error(), kExitFailure);
                    }
                    found = true;
                }
            }
            if (!found) {
                return output.flushError(
                    makeErrorValue(ErrorCode::NotFound, "no such folder id in this account"),
                    kExitNotFound);
            }
            folders = store.folders(accountId);
        }
        QList<QStringList> table;
        for (const mail::MailFolderRecord& folder : folders.value()) {
            table.append({folder.remoteId, folder.displayName,
                          folder.syncEnabled ? QStringLiteral("yes") : QStringLiteral("no"),
                          folder.deltaLink.isEmpty() ? QStringLiteral("-")
                                                     : QStringLiteral("set")});
        }
        output.add(QStringLiteral("ok"), true);
        output.add(QStringLiteral("count"), static_cast<int>(table.size()));
        output.table({QStringLiteral("REMOTE ID"), QStringLiteral("FOLDER"),
                      QStringLiteral("SYNC"), QStringLiteral("DELTA")},
                     table);
        return output.flush(stdout, kExitSuccess);
    }

    if (sub == QStringLiteral("estimate")) {
        // Count first, sync second: the folder counters come from Microsoft, so
        // the size of the job is known before a single message is downloaded.
        bool refreshed = false;
        const Result<std::shared_ptr<mail::GraphClient>> graph =
            openGraph(context, account.value());
        if (graph) {
            const Result<QVector<mail::MailFolderInfo>> folders = graph.value()->mailFolders();
            if (folders) {
                store.storeFolders(accountId, folders.value());
                refreshed = true;
            }
        }
        const Result<QVector<mail::MailFolderStats>> stats = store.folderStats(accountId);
        if (!stats) {
            return output.flushError(stats.error(), kExitFailure);
        }

        QList<QStringList> table;
        qint64 total = 0;
        qint64 archived = 0;
        int enabledFolders = 0;
        for (const mail::MailFolderStats& folder : stats.value()) {
            if (!folder.syncEnabled) {
                continue;
            }
            ++enabledFolders;
            total += folder.totalItems;
            archived += folder.exportedItems;
            table.append({folder.displayName, QString::number(folder.totalItems),
                          QString::number(folder.exportedItems),
                          QString::number(folder.remainingItems())});
        }
        const qint64 remaining = qMax<qint64>(0, total - archived);

        output.add(QStringLiteral("ok"), true);
        output.add(QStringLiteral("account"), account.value().accountIdentifier);
        output.add(QStringLiteral("folders"), enabledFolders);
        output.add(QStringLiteral("total_messages"), total);
        output.add(QStringLiteral("archived_messages"), archived);
        output.add(QStringLiteral("remaining_messages"), remaining);
        output.add(QStringLiteral("counts_refreshed"), refreshed);
        output.table({QStringLiteral("FOLDER"), QStringLiteral("IN MICROSOFT"),
                      QStringLiteral("ARCHIVED"), QStringLiteral("REMAINING")},
                     table);
        output.line(QStringLiteral("%1 messages in %2 enabled folder(s); %3 archived, %4 to go.")
                        .arg(total)
                        .arg(enabledFolders)
                        .arg(archived)
                        .arg(remaining));
        if (!refreshed) {
            output.line(QStringLiteral("Folder counts are the stored ones: Microsoft could not be "
                                       "asked just now, so they may be slightly out of date."));
        }

        if (remaining == 0) {
            output.line(QStringLiteral("Nothing left to archive in the enabled folders."));
            output.add(QStringLiteral("eta_seconds"), 0);
            return output.flush(stdout, kExitSuccess);
        }
        if (!account.value().hasSpeed()) {
            output.line(QStringLiteral("No measured speed yet: run `mail sync` once and later "
                                       "estimates will carry a duration."));
            output.add(QStringLiteral("eta_seconds"), -1);
            return output.flush(stdout, kExitSuccess);
        }

        const double rate = account.value().messagesPerSecond();
        const qint64 etaSeconds = rate > 0 ? static_cast<qint64>(remaining / rate) : -1;
        const qint64 bytes = remaining * account.value().bytesPerMessage();
        output.add(QStringLiteral("eta_seconds"), etaSeconds);
        output.add(QStringLiteral("expected_bytes"), bytes);
        output.add(QStringLiteral("messages_per_second"), rate);
        output.line(QStringLiteral("At the measured %1 messages/s of the last sync: about %2 "
                                   "(~%3) remaining.")
                        .arg(rate, 0, 'f', 1)
                        .arg(humanDuration(etaSeconds))
                        .arg(humanBytes(bytes)));
        if (bytes == 0) {
            output.line(QStringLiteral("The expected size is unknown until a sync has downloaded "
                                       "something."));
        }
        return output.flush(stdout, kExitSuccess);
    }

    if (sub == QStringLiteral("sync")) {
        if (configuration.destination.trimmed().isEmpty()) {
            return output.flushError(
                makeErrorValue(ErrorCode::InvalidArgument,
                               QStringLiteral("mail sync requires --to <directory> "
                                              "(or set mail.destination)")
                                   .toStdString()),
                kExitUsage);
        }
        Result<std::shared_ptr<mail::GraphClient>> graph = openGraph(context, account.value());
        if (!graph) {
            return output.flushError(graph.error(), kExitFailure);
        }
        const Result<QVector<mail::MailFolderInfo>> folders = graph.value()->mailFolders();
        if (folders) {
            store.storeFolders(accountId, folders.value());
        }

        mail::MailExportOptions exportOptions;
        exportOptions.destinationDirectory = configuration.destination;
        exportOptions.fullResync = options.flags.contains(QStringLiteral("full"));
        exportOptions.maxMessages = options.flags.contains(QStringLiteral("max-messages"))
                                        ? options.flags.value(QStringLiteral("max-messages")).toInt()
                                        : 0;
        if (options.flags.contains(QStringLiteral("folder"))) {
            exportOptions.folderRemoteIds.append(options.flags.value(QStringLiteral("folder")));
        }

        const auto progress = [quiet = options.quiet](const mail::MailProgress& update) {
            if (quiet) {
                return;
            }
            QString line;
            if (update.percent() >= 0.0) {
                line = QStringLiteral("[%1%] %2/%3 messages")
                           .arg(update.percent(), 5, 'f', 1)
                           .arg(update.done)
                           .arg(update.total);
            } else {
                // A delta run reports what changed, so a percentage of the
                // mailbox would be meaningless.
                line = QStringLiteral("[delta] %1 messages examined").arg(update.done);
            }
            if (update.messagesPerSecond > 0.0) {
                line += QStringLiteral(" · %1 msg/s").arg(update.messagesPerSecond, 0, 'f', 1);
            }
            if (update.etaSeconds >= 0) {
                line += QStringLiteral(" · ETA %1").arg(humanDuration(update.etaSeconds));
            }
            if (update.bytesWritten > 0) {
                line += QStringLiteral(" · %1 archived").arg(humanBytes(update.bytesWritten));
            }
            line += QStringLiteral(" · %1").arg(update.folder);
            renderMailProgress(line);
        };
        mail::MailExporter exporter(context.database, graph.value(), progress);
        const CancellationToken cancellation;
        const Result<mail::MailExportReport> report =
            exporter.run(accountId, exportOptions, cancellation);
        if (!options.quiet) {
            std::fprintf(stderr, "\n");
        }
        if (!report) {
            return output.flushError(report.error(), kExitFailure);
        }
        output.add(QStringLiteral("ok"), report.value().succeeded());
        output.add(QStringLiteral("folders"), report.value().folders);
        output.add(QStringLiteral("exported"), report.value().exported);
        output.add(QStringLiteral("skipped"), report.value().skipped);
        output.add(QStringLiteral("removed"), report.value().removed);
        output.add(QStringLiteral("failed"), report.value().failed);
        output.add(QStringLiteral("bytes"), report.value().bytesWritten);
        output.line(QStringLiteral("Exported %1, skipped %2, removed %3, failed %4 (%5)")
                        .arg(report.value().exported)
                        .arg(report.value().skipped)
                        .arg(report.value().removed)
                        .arg(report.value().failed)
                        .arg(humanBytes(report.value().bytesWritten)));
        const qint64 elapsed = static_cast<qint64>(report.value().elapsedSeconds);
        output.line(QStringLiteral("Took %1 at %2 messages/s.")
                        .arg(humanDuration(elapsed))
                        .arg(report.value().messagesPerSecond(), 0, 'f', 1));
        if (report.value().incremental) {
            output.line(QStringLiteral("Incremental run: only messages that changed were examined."));
        } else {
            const qint64 inScope = qMax<qint64>(1, report.value().totalMessages);
            output.line(QStringLiteral("Coverage: %1 of %2 messages in scope (%3%), %4 archived.")
                            .arg(report.value().messagesSeen)
                            .arg(report.value().totalMessages)
                            .arg(100.0 * report.value().messagesSeen / inScope, 0, 'f', 1)
                            .arg(humanBytes(report.value().bytesWritten)));
        }
        for (const QString& error : report.value().errors) {
            output.line(QStringLiteral("  error: %1").arg(error));
        }
        return output.flush(stdout, report.value().succeeded() ? kExitSuccess : kExitFailure);
    }

    return output.flushError(
        makeErrorValue(ErrorCode::InvalidArgument,
                       QStringLiteral("unknown mail command: %1").arg(sub).toStdString()),
        kExitUsage);
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
    if (command == QStringLiteral("tools")) {
        return commandTools(context, options);
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
    if (command == QStringLiteral("mail")) {
        return commandMail(context, options);
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
