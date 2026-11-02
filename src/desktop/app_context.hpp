// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>
#include <optional>

#include "nesva/config/settings_store.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/core/result.hpp"
#include "nesva/db/database.hpp"
#include "theme.hpp"

namespace nesva::ui {

struct StartupOptions {
    QString dataRootOverride;
    std::optional<LogLevel> consoleLogLevel;
    bool consoleLogging = true;
};

/// Owns the process-wide services used by the desktop application:
/// database connection, settings store, logging configuration and theme state.
///
/// All database access happens on the GUI thread in this phase; long-running
/// work moves to worker threads in later phases.
class AppContext : public QObject {
    Q_OBJECT

public:
    explicit AppContext(QObject* parent = nullptr);
    ~AppContext() override;

    /// Opens the database, applies migrations and configures logging.
    Status initialise(const StartupOptions& options);

    db::Database& database() { return database_; }
    config::SettingsStore& settings() { return *settings_; }
    const QString& logFilePath() const { return logFilePath_; }

    ThemeMode themeMode() const;
    Status setThemeMode(ThemeMode mode);

    /// Persists a diagnostic event into application_events and the log file.
    void recordEvent(LogLevel level, const QString& category, const QString& message,
                     const QJsonObject& data = {});

    /// One-line summary for the top status bar.
    QString healthSummary();

signals:
    void themeModeChanged();
    void statusChanged();

private:
    db::Database database_;
    std::unique_ptr<config::SettingsStore> settings_;
    QString logFilePath_;
};

} // namespace nesva::ui
