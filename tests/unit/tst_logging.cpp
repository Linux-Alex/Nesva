// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>

#include <memory>

#include "nesva/core/logging.hpp"

using namespace nesva;

namespace {

QList<QJsonObject> readJsonLines(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QList<QJsonObject> records;
    const QByteArray content = file.readAll();
    for (const QByteArray& line : content.split('\n')) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            records.append(QJsonObject{{QStringLiteral("__parse_error"), QString::fromUtf8(line)}});
            continue;
        }
        records.append(document.object());
    }
    return records;
}

} // namespace

class LoggingTest : public QObject {
    Q_OBJECT

private slots:
    void levelTokensRoundTrip();
    void writesStructuredJsonLines();
    void filtersBelowMinimumLevel();
    void redactsSecretsBeforeWriting();
    void rotatesWhenFileGrows();
    void formatsConsoleLines();
    void reopenAfterExternalDeletion();
};

void LoggingTest::levelTokensRoundTrip() {
    QCOMPARE(QString::fromUtf8(toToken(LogLevel::Warn)), QStringLiteral("warn"));
    QCOMPARE(static_cast<int>(logLevelFromString("error").value_or(LogLevel::Off)), static_cast<int>(LogLevel::Error));
    QVERIFY(!logLevelFromString("nonsense").has_value());
    QCOMPARE(toDisplayString(LogLevel::Critical), QStringLiteral("CRITICAL"));
}

void LoggingTest::writesStructuredJsonLines() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    auto sink = std::make_shared<FileSink>(directory.filePath(QStringLiteral("nesva.log")));
    QVERIFY(sink->open().ok());

    Logger logger;
    logger.addSink(sink);
    logger.setMinLevel(LogLevel::Trace);
    logger.log(LogLevel::Info, QStringLiteral("core.test"), QStringLiteral("hello"),
               QJsonObject{{QStringLiteral("count"), 3}});
    logger.log(LogLevel::Error, QStringLiteral("core.test"), QStringLiteral("bad"));
    logger.flush();

    const QList<QJsonObject> records =
        readJsonLines(directory.filePath(QStringLiteral("nesva.log")));
    QCOMPARE(records.size(), 2);

    QCOMPARE(records.at(0).value(QStringLiteral("level")).toString(), QStringLiteral("info"));
    QCOMPARE(records.at(0).value(QStringLiteral("category")).toString(), QStringLiteral("core.test"));
    QCOMPARE(records.at(0).value(QStringLiteral("message")).toString(), QStringLiteral("hello"));
    QCOMPARE(records.at(0).value(QStringLiteral("fields")).toObject().value(QStringLiteral("count")).toInt(), 3);
    QVERIFY(records.at(0).value(QStringLiteral("timestamp")).toString().endsWith(QLatin1Char('Z')));
    QCOMPARE(records.at(1).value(QStringLiteral("level")).toString(), QStringLiteral("error"));
}

void LoggingTest::filtersBelowMinimumLevel() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    auto sink = std::make_shared<FileSink>(directory.filePath(QStringLiteral("filtered.log")));
    QVERIFY(sink->open().ok());

    Logger logger;
    logger.addSink(sink);
    logger.setMinLevel(LogLevel::Error);
    logger.log(LogLevel::Info, QStringLiteral("core.test"), QStringLiteral("skipped"));
    logger.log(LogLevel::Error, QStringLiteral("core.test"), QStringLiteral("kept"));
    logger.flush();

    const QList<QJsonObject> records =
        readJsonLines(directory.filePath(QStringLiteral("filtered.log")));
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.at(0).value(QStringLiteral("message")).toString(), QStringLiteral("kept"));
}

void LoggingTest::redactsSecretsBeforeWriting() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    auto sink = std::make_shared<FileSink>(directory.filePath(QStringLiteral("secret.log")));
    QVERIFY(sink->open().ok());

    Logger logger;
    logger.addSink(sink);
    logger.setMinLevel(LogLevel::Trace);
    logger.addSecret(QStringLiteral("s3cr3t-rclone-token"));

    logger.log(LogLevel::Info, QStringLiteral("provider.icloud"),
               QStringLiteral("authenticating with s3cr3t-rclone-token"),
               QJsonObject{{QStringLiteral("password"), QStringLiteral("hunter2")},
                           {QStringLiteral("note"), QStringLiteral("bearer abcdefghijklmnop")}});
    logger.flush();

    QFile file(directory.filePath(QStringLiteral("secret.log")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString content = QString::fromUtf8(file.readAll());
    QVERIFY(!content.contains(QStringLiteral("s3cr3t-rclone-token")));
    QVERIFY(!content.contains(QStringLiteral("hunter2")));
    QVERIFY(!content.contains(QStringLiteral("abcdefghijklmnop")));
    QVERIFY(content.contains(QString::fromUtf8(Redactor::mask().toUtf8())));

    const QList<QJsonObject> records =
        readJsonLines(directory.filePath(QStringLiteral("secret.log")));
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.at(0).value(QStringLiteral("fields")).toObject().value(QStringLiteral("password")).toString(),
             Redactor::mask());
}

void LoggingTest::rotatesWhenFileGrows() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("rotating.log"));

    auto sink = std::make_shared<FileSink>(path, 600, 3);
    QVERIFY(sink->open().ok());

    Logger logger;
    logger.addSink(sink);
    logger.setMinLevel(LogLevel::Trace);
    for (int index = 0; index < 80; ++index) {
        logger.log(LogLevel::Info, QStringLiteral("core.rotate"),
                   QStringLiteral("record number %1 with padding xxxxxxxxxxxxxxxxxxxxxxxxx").arg(index));
    }
    logger.flush();

    QVERIFY(QFile::exists(path));
    QVERIFY2(QFile::exists(path + QStringLiteral(".1")), "expected at least one rotated file");

    const QList<QJsonObject> current = readJsonLines(path);
    QVERIFY(!current.isEmpty());
    QVERIFY2(QFileInfo(path).size() <= 1200, "active log file stays bounded");

    // Newest records are always in the active file.
    const QString lastMessage = current.last().value(QStringLiteral("message")).toString();
    QVERIFY(lastMessage.contains(QStringLiteral("record number")));
}

void LoggingTest::formatsConsoleLines() {
    LogRecord record;
    record.timestampUtc = QDateTime::currentDateTimeUtc();
    record.level = LogLevel::Warn;
    record.category = QStringLiteral("provider.rclone");
    record.message = QStringLiteral("retrying transfer");
    record.fields.insert(QStringLiteral("attempt"), 2);

    const QString line = formatForConsole(record);
    QVERIFY(line.contains(QStringLiteral("[WARN")));
    QVERIFY(line.contains(QStringLiteral("provider.rclone: retrying transfer")));
    QVERIFY(line.contains(QStringLiteral("attempt=2")));
}

void LoggingTest::reopenAfterExternalDeletion() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("reopen.log"));

    auto sink = std::make_shared<FileSink>(path);
    QVERIFY(sink->open().ok());

    Logger logger;
    logger.addSink(sink);
    logger.setMinLevel(LogLevel::Trace);
    logger.log(LogLevel::Info, QStringLiteral("core.test"), QStringLiteral("first"));
    logger.flush();

    QVERIFY(QFile::remove(path));
    // The sink keeps writing even if the file disappeared (rotation/cleanup races).
    logger.log(LogLevel::Info, QStringLiteral("core.test"), QStringLiteral("second"));
    logger.flush();

    const QList<QJsonObject> records = readJsonLines(path);
    QVERIFY(!records.isEmpty());
    QCOMPARE(records.last().value(QStringLiteral("message")).toString(), QStringLiteral("second"));
}

QTEST_GUILESS_MAIN(LoggingTest)
#include "tst_logging.moc"
