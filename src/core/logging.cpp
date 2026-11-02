// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/core/logging.hpp"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonValue>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "nesva/core/paths.hpp"

namespace nesva {

std::string_view toToken(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    case LogLevel::Critical:
        return "critical";
    case LogLevel::Off:
        return "off";
    }
    return "info";
}

QString toDisplayString(LogLevel level) {
    QString text = QString::fromUtf8(toToken(level));
    return text.toUpper();
}

std::optional<LogLevel> logLevelFromString(std::string_view name) noexcept {
    if (name == "trace") {
        return LogLevel::Trace;
    }
    if (name == "debug") {
        return LogLevel::Debug;
    }
    if (name == "info") {
        return LogLevel::Info;
    }
    if (name == "warn" || name == "warning") {
        return LogLevel::Warn;
    }
    if (name == "error") {
        return LogLevel::Error;
    }
    if (name == "critical" || name == "fatal") {
        return LogLevel::Critical;
    }
    if (name == "off" || name == "none") {
        return LogLevel::Off;
    }
    return std::nullopt;
}

QString formatForConsole(const LogRecord& record) {
    QString line = record.timestampUtc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    line += QStringLiteral(" [");
    line += toDisplayString(record.level).leftJustified(8);
    line += QStringLiteral("] ");
    if (!record.category.isEmpty()) {
        line += record.category;
        line += QStringLiteral(": ");
    }
    line += record.message;

    if (!record.fields.isEmpty()) {
        QStringList parts;
        parts.reserve(record.fields.size());
        for (auto it = record.fields.begin(); it != record.fields.end(); ++it) {
            const QJsonValue& value = it.value();
            QString rendered;
            if (value.isString()) {
                rendered = value.toString();
            } else if (value.isBool()) {
                rendered = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
            } else if (value.isDouble()) {
                rendered = QString::number(value.toDouble());
            } else if (value.isNull()) {
                rendered = QStringLiteral("null");
            } else if (value.isArray()) {
                rendered = QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
            } else {
                rendered = QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
            }
            if (rendered.contains(QLatin1Char(' ')) || rendered.isEmpty()) {
                rendered = QLatin1Char('"') + rendered + QLatin1Char('"');
            }
            parts.append(it.key() + QLatin1Char('=') + rendered);
        }
        line += QLatin1Char(' ') + parts.join(QLatin1Char(' '));
    }
    return line;
}

// ---------------------------------------------------------------------------
// StreamSink
// ---------------------------------------------------------------------------

StreamSink::StreamSink(bool toStandardError) : toStandardError_(toStandardError) {}

void StreamSink::write(const LogRecord& record) {
    const QByteArray utf8 = formatForConsole(record).toUtf8();
    std::FILE* stream = toStandardError_ ? stderr : stdout;
    std::fwrite(utf8.constData(), 1, static_cast<std::size_t>(utf8.size()), stream);
    std::fputc('\n', stream);
    std::fflush(stream);
}

void StreamSink::flush() {
    std::fflush(toStandardError_ ? stderr : stdout);
}

// ---------------------------------------------------------------------------
// FileSink
// ---------------------------------------------------------------------------

FileSink::FileSink(QString filePath, qint64 maxFileBytes, int maxBackupFiles)
    : filePath_(std::move(filePath)),
      maxFileBytes_(maxFileBytes),
      maxBackupFiles_(maxBackupFiles) {}

FileSink::~FileSink() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_) {
        file_.flush();
        file_.close();
        open_ = false;
    }
}

Status FileSink::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.isOpen()) {
        file_.flush();
        file_.close();
    }
    open_ = false;

    const QFileInfo info(filePath_);
    const QDir directory = info.absoluteDir();
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        lastError_ = QStringLiteral("cannot create log directory: %1").arg(directory.absolutePath());
        return makeError(ErrorCode::IoError, "cannot create log directory",
                         directory.absolutePath().toStdString());
    }

    file_.setFileName(filePath_);
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Append)) {
        lastError_ = file_.errorString();
        return makeError(ErrorCode::IoError, "cannot open log file for writing",
                         lastError_.toStdString());
    }

    bytesWritten_ = file_.size();
    lastError_.clear();
    open_ = true;
    return Status();
}

Status FileSink::rotateLocked(qint64 incomingBytes) {
    if (maxFileBytes_ <= 0 || bytesWritten_ <= 0 || maxBackupFiles_ <= 0) {
        return Status();
    }
    if (bytesWritten_ + incomingBytes <= maxFileBytes_) {
        return Status();
    }

    file_.flush();
    file_.close();
    open_ = false;

    if (maxBackupFiles_ == 1) {
        QFile::remove(filePath_);
    } else {
        QFile::remove(QStringLiteral("%1.%2").arg(filePath_).arg(maxBackupFiles_));
        for (int index = maxBackupFiles_ - 1; index >= 1; --index) {
            const QString from = QStringLiteral("%1.%2").arg(filePath_).arg(index);
            const QString to = QStringLiteral("%1.%2").arg(filePath_).arg(index + 1);
            if (QFile::exists(from) && !QFile::rename(from, to)) {
                // A failed rotation must not lose the current log file: keep
                // appending to it instead of failing the write.
                lastError_ = QStringLiteral("log rotation failed for %1").arg(from);
            }
        }
        QFile::rename(filePath_, QStringLiteral("%1.1").arg(filePath_));
    }

    file_.setFileName(filePath_);
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Append)) {
        lastError_ = file_.errorString();
        return makeError(ErrorCode::IoError, "cannot reopen rotated log file",
                         lastError_.toStdString());
    }
    bytesWritten_ = 0;
    open_ = true;
    return Status();
}

void FileSink::write(const LogRecord& record) {
    QJsonObject object;
    object.insert(QStringLiteral("timestamp"),
                  record.timestampUtc.toUTC().toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("level"), QString::fromUtf8(toToken(record.level)));
    object.insert(QStringLiteral("category"), record.category);
    object.insert(QStringLiteral("message"), record.message);
    if (!record.fields.isEmpty()) {
        object.insert(QStringLiteral("fields"), record.fields);
    }
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';

    std::lock_guard<std::mutex> lock(mutex_);
    // Detect a log file removed underneath us (external cleanup or rotation by
    // another tool) and start a new one instead of writing into an unlinked inode.
    if (open_ && !QFile::exists(filePath_)) {
        file_.close();
        open_ = false;
        bytesWritten_ = 0;
    }
    if (!open_) {
        file_.setFileName(filePath_);
        if (!file_.open(QIODevice::WriteOnly | QIODevice::Append)) {
            lastError_ = file_.errorString();
            return;
        }
        bytesWritten_ = file_.size();
        open_ = true;
    }

    if (!rotateLocked(static_cast<qint64>(line.size()))) {
        // Rotation failed; keep writing to the existing file rather than
        // dropping the record. The failure is surfaced by lastError().
    }

    const qint64 written = file_.write(line);
    if (written > 0) {
        bytesWritten_ += written;
    }
    file_.flush();
}

void FileSink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_) {
        file_.flush();
    }
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

void Logger::addSink(std::shared_ptr<LogSink> sink) {
    if (!sink) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::clearSinks() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sink : sinks_) {
        if (sink) {
            sink->flush();
        }
    }
    sinks_.clear();
}

std::size_t Logger::sinkCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sinks_.size();
}

void Logger::setMinLevel(LogLevel level) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    minLevel_ = level;
}

LogLevel Logger::minLevel() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return minLevel_;
}

bool Logger::isEnabled(LogLevel level) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return level >= minLevel_ && level != LogLevel::Off;
}

void Logger::log(LogLevel level, QString category, QString message, QJsonObject fields) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (level < minLevel_ || level == LogLevel::Off) {
        return;
    }

    LogRecord record;
    record.timestampUtc = QDateTime::currentDateTimeUtc();
    record.level = level;
    record.category = std::move(category);
    record.message = redactor_.redact(message);
    record.fields = redactor_.redactObject(fields);

    for (auto& sink : sinks_) {
        if (sink && sink->accepts(level)) {
            sink->write(record);
        }
    }
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sink : sinks_) {
        if (sink) {
            sink->flush();
        }
    }
}

void Logger::addSecret(const QString& secret) {
    std::lock_guard<std::mutex> lock(mutex_);
    redactor_.addSecret(secret);
}

void Logger::clearSecrets() {
    std::lock_guard<std::mutex> lock(mutex_);
    redactor_.clearSecrets();
}

Logger& globalLogger() {
    static Logger logger;
    return logger;
}

Result<QString> initialiseLogging(const LoggingOptions& options) {
    QString directory = options.logDirectory;
    if (directory.isEmpty()) {
        directory = Paths::logsDirectory();
    }

    QDir dir(directory);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return makeErrorValue(ErrorCode::IoError, "cannot create log directory",
                              directory.toStdString());
    }

    auto fileSink = std::make_shared<FileSink>(dir.absoluteFilePath(options.fileName),
                                               options.maxFileBytes, options.maxBackupFiles);
    const Status opened = fileSink->open();
    if (!opened) {
        return opened.error();
    }

    Logger& logger = globalLogger();
    logger.clearSinks();

    fileSink->setMinimumLevel(options.fileLevel);
    logger.addSink(fileSink);

    // The logger's own minimum is the most permissive configured level; each
    // sink then filters independently so console verbosity is decoupled from
    // the diagnostic file.
    LogLevel mostPermissive = options.fileLevel;
    if (options.consoleEnabled) {
        auto streamSink = std::make_shared<StreamSink>(options.consoleToStandardError);
        streamSink->setMinimumLevel(options.consoleLevel);
        logger.addSink(streamSink);
        if (options.consoleLevel < mostPermissive) {
            mostPermissive = options.consoleLevel;
        }
    }
    logger.setMinLevel(mostPermissive);

    return fileSink->filePath();
}

void logMessage(LogLevel level, std::string_view category, const QString& message,
                QJsonObject fields) {
    globalLogger().log(level, QString::fromUtf8(category), message, std::move(fields));
}

void logTrace(std::string_view category, const QString& message, QJsonObject fields) {
    logMessage(LogLevel::Trace, category, message, std::move(fields));
}

void logDebug(std::string_view category, const QString& message, QJsonObject fields) {
    logMessage(LogLevel::Debug, category, message, std::move(fields));
}

void logInfo(std::string_view category, const QString& message, QJsonObject fields) {
    logMessage(LogLevel::Info, category, message, std::move(fields));
}

void logWarn(std::string_view category, const QString& message, QJsonObject fields) {
    logMessage(LogLevel::Warn, category, message, std::move(fields));
}

void logError(std::string_view category, const QString& message, QJsonObject fields) {
    logMessage(LogLevel::Error, category, message, std::move(fields));
}

void logCritical(std::string_view category, const QString& message, QJsonObject fields) {
    logMessage(LogLevel::Critical, category, message, std::move(fields));
}

} // namespace nesva
