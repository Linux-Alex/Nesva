// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "app_context.hpp"

#include <QtCore/QJsonDocument>
#include <QtCore/QVariant>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include <utility>

#include "nesva/core/app_info.hpp"
#include "nesva/core/paths.hpp"
#include "nesva/jobs/job_store.hpp"

namespace nesva::ui {
namespace {

QString levelName(LogLevel level) {
    return QString::fromUtf8(toToken(level));
}

} // namespace

AppContext::AppContext(QObject* parent) : QObject(parent) {}

AppContext::~AppContext() {
    if (executor_) {
        executor_->waitForFinished();
        executor_.reset();
    }
    if (settings_) {
        recordEvent(LogLevel::Info, QStringLiteral("application"),
                    QStringLiteral("application stopped"));
    }
    globalLogger().flush();
    database_.close();
}

Status AppContext::initialise(const StartupOptions& options) {
    if (!options.dataRootOverride.isEmpty()) {
        Paths::setDataRootOverride(options.dataRootOverride);
    }

    Status directories = Paths::ensureBaseDirectories();
    if (!directories) {
        return directories;
    }

    LoggingOptions logging;
    logging.logDirectory = Paths::logsDirectory();
    logging.fileLevel = LogLevel::Debug;
    logging.consoleEnabled = options.consoleLogging;
    logging.consoleLevel = options.consoleLogLevel.value_or(LogLevel::Info);
    logging.consoleToStandardError = true;

    Result<QString> logFile = initialiseLogging(logging);
    if (!logFile) {
        return logFile.error();
    }
    logFilePath_ = logFile.value();

    Status opened = database_.open(Paths::databasePath());
    if (!opened) {
        return opened;
    }

    Status migrated = database_.migrate();
    if (!migrated) {
        return migrated;
    }

    settings_ = std::make_unique<config::SettingsStore>(database_);

    const Result<QString> configuredLevel =
        settings_->getString(QString::fromUtf8(config::keys::kLogLevel));
    if (configuredLevel && !configuredLevel.value().isEmpty()) {
        if (const auto parsed = logLevelFromString(configuredLevel.value().toStdString())) {
            globalLogger().setMinLevel(*parsed);
        }
    }

    // Recover runs whose owning process died (crash, kill, power loss) before
    // the UI reports anything as running.
    jobs::JobStore store(database_);
    const Result<int> recovered = store.recoverStaleRuns();
    if (recovered && recovered.value() > 0) {
        recordEvent(LogLevel::Warn, QStringLiteral("application"),
                    QStringLiteral("%1 interrupted run(s) recovered after an unclean shutdown")
                        .arg(recovered.value()));
    }

    executor_ = std::make_unique<JobExecutor>(*this);

    recordEvent(LogLevel::Info, QStringLiteral("application"),
                QStringLiteral("application started"),
                QJsonObject{{QStringLiteral("version"), ApplicationInfo::version()},
                            {QStringLiteral("data_root"), Paths::dataRoot()}});
    return Status();
}

ThemeMode AppContext::themeMode() const {
    if (!settings_) {
        return ThemeMode::System;
    }
    const Result<QString> value =
        settings_->getString(QString::fromUtf8(config::keys::kThemeMode));
    if (!value) {
        return ThemeMode::System;
    }
    return themeModeFromString(value.value()).value_or(ThemeMode::System);
}

Status AppContext::setThemeMode(ThemeMode mode) {
    if (!settings_) {
        return makeError(ErrorCode::Internal, "settings store is not initialised");
    }
    Status stored = settings_->setString(QString::fromUtf8(config::keys::kThemeMode),
                                         themeModeToString(mode));
    if (!stored) {
        return stored;
    }
    emit themeModeChanged();
    return Status();
}

void AppContext::recordEvent(LogLevel level, const QString& category, const QString& message,
                             const QJsonObject& data) {
    globalLogger().log(level, category, message, data);

    if (!database_.isOpen()) {
        return;
    }

    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "INSERT INTO application_events (level, category, message, data_json, created_at) "
        "VALUES (?, ?, ?, ?, ?)"));
    query.addBindValue(levelName(level));
    query.addBindValue(category);
    query.addBindValue(message);
    query.addBindValue(data.isEmpty() ? QVariant()
                                      : QVariant(QString::fromUtf8(
                                            QJsonDocument(data).toJson(QJsonDocument::Compact))));
    query.addBindValue(db::Database::currentTimestampUtc());
    if (!query.exec()) {
        logWarn("ui.events", QStringLiteral("could not persist application event"),
                QJsonObject{{QStringLiteral("error"), query.lastError().text()}});
    }
}

QString AppContext::healthSummary() {
    if (!database_.isOpen()) {
        return QStringLiteral("Database unavailable");
    }

    Result<int> schema = database_.schemaVersion();
    if (!schema) {
        return QStringLiteral("Database error");
    }
    if (schema.value() != db::Database::latestSchemaVersion()) {
        return QStringLiteral("Schema out of date");
    }

    Result<QSqlQuery> jobsQuery =
        database_.select(QStringLiteral("SELECT COUNT(*) FROM backup_jobs"));
    if (!jobsQuery) {
        return QStringLiteral("Database error");
    }
    QSqlQuery jobs = std::move(jobsQuery).take();
    const int jobCount = jobs.next() ? jobs.value(0).toInt() : 0;

    Result<QSqlQuery> storageQuery =
        database_.select(QStringLiteral("SELECT COUNT(*) FROM storage_providers"));
    if (!storageQuery) {
        return QStringLiteral("Database error");
    }
    QSqlQuery storage = std::move(storageQuery).take();
    const int storageCount = storage.next() ? storage.value(0).toInt() : 0;

    if (jobCount == 0) {
        return QStringLiteral("Ready \u2014 no jobs configured yet");
    }
    return QStringLiteral("Ready \u2014 %1 job(s), %2 storage provider(s)")
        .arg(jobCount)
        .arg(storageCount);
}

} // namespace nesva::ui
