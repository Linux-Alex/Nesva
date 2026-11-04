// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "nesva/core/redactor.hpp"
#include "nesva/core/result.hpp"

namespace nesva {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5,
    Off = 6,
};

/// Lower-case token used in JSON log files ("info", "warn", ...).
std::string_view toToken(LogLevel level) noexcept;
QString toDisplayString(LogLevel level);
std::optional<LogLevel> logLevelFromString(std::string_view name) noexcept;

/// One structured log event.
struct LogRecord {
    QDateTime timestampUtc;
    LogLevel level = LogLevel::Info;
    QString category;
    QString message;
    QJsonObject fields;
};

/// Destination for log records. Sinks are called while the logger holds its
/// internal mutex, so they must not log or block on user interaction.
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() {}

    /// Records below this level are filtered out for this sink only, so the
    /// console can stay quiet while the log file keeps full diagnostic detail.
    void setMinimumLevel(LogLevel level) noexcept { minimumLevel_ = level; }
    LogLevel minimumLevel() const noexcept { return minimumLevel_; }
    bool accepts(LogLevel level) const noexcept {
        return level >= minimumLevel_ && level != LogLevel::Off;
    }

private:
    LogLevel minimumLevel_ = LogLevel::Trace;
};

/// Human-readable sink used for console output of the CLI and the agent.
class StreamSink final : public LogSink {
public:
    explicit StreamSink(bool toStandardError = true);

    void write(const LogRecord& record) override;
    void flush() override;

private:
    bool toStandardError_ = true;
};

/// Newline-delimited JSON sink with size-based rotation.
///
/// Each record is written as a single JSON object per line so that log files
/// can be processed by standard tooling and exported as diagnostics without a
/// custom parser.
class FileSink final : public LogSink {
public:
    FileSink(QString filePath, qint64 maxFileBytes = 4 * 1024 * 1024, int maxBackupFiles = 5);
    ~FileSink() override;

    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;

    /// Opens (or reopens) the log file, creating parent directories.
    Status open();
    /// Flushes and releases the file handle. Windows refuses to delete a file a
    /// process still holds open, so closing is the only way an external tool can
    /// remove or rotate the log; the next write reopens it.
    void close();
    bool isOpen() const noexcept { return open_; }
    const QString& filePath() const noexcept { return filePath_; }
    const QString& lastError() const noexcept { return lastError_; }
    qint64 bytesWritten() const noexcept { return bytesWritten_; }

    void write(const LogRecord& record) override;
    void flush() override;

private:
    Status rotateLocked(qint64 incomingBytes);

    mutable std::mutex mutex_;
    QFile file_;
    QString filePath_;
    qint64 maxFileBytes_ = 4 * 1024 * 1024;
    int maxBackupFiles_ = 5;
    QByteArray pending_;
    bool open_ = false;
    QString lastError_;
    qint64 bytesWritten_ = 0;
};

/// Central logger. Not a singleton: applications use globalLogger(), tests
/// construct isolated instances with their own sinks.
class Logger {
public:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void addSink(std::shared_ptr<LogSink> sink);
    void clearSinks();
    std::size_t sinkCount() const;

    void setMinLevel(LogLevel level) noexcept;
    LogLevel minLevel() const noexcept;
    bool isEnabled(LogLevel level) const noexcept;

    void log(LogLevel level, QString category, QString message, QJsonObject fields = {});
    void flush();

    /// Registers a literal secret that must never appear in any sink.
    void addSecret(const QString& secret);
    void clearSecrets();

    Redactor& redactor() noexcept { return redactor_; }

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<LogSink>> sinks_;
    LogLevel minLevel_ = LogLevel::Info;
    Redactor redactor_;
};

/// Process-wide logger used by the applications.
Logger& globalLogger();

/// Options for initialiseLogging().
struct LoggingOptions {
    QString logDirectory;
    LogLevel fileLevel = LogLevel::Debug;
    LogLevel consoleLevel = LogLevel::Info;
    bool consoleEnabled = false;
    bool consoleToStandardError = true;
    qint64 maxFileBytes = 4 * 1024 * 1024;
    int maxBackupFiles = 5;
    QString fileName = QStringLiteral("nesva.log");
};

/// Configures the global logger with a rotating JSON file sink and an optional
/// console sink. Returns the absolute path of the active log file.
Result<QString> initialiseLogging(const LoggingOptions& options);

/// Convenience helpers writing to globalLogger().
void logMessage(LogLevel level, std::string_view category, const QString& message,
                QJsonObject fields = {});
void logTrace(std::string_view category, const QString& message, QJsonObject fields = {});
void logDebug(std::string_view category, const QString& message, QJsonObject fields = {});
void logInfo(std::string_view category, const QString& message, QJsonObject fields = {});
void logWarn(std::string_view category, const QString& message, QJsonObject fields = {});
void logError(std::string_view category, const QString& message, QJsonObject fields = {});
void logCritical(std::string_view category, const QString& message, QJsonObject fields = {});

/// Renders a record in the human-readable console format.
QString formatForConsole(const LogRecord& record);

} // namespace nesva
