// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include "nesva/agent/protocol.hpp"
#include "nesva/core/paths.hpp"
#include "support/test_paths.hpp"

using namespace nesva;
using namespace nesva::agent;

class AgentProtocolTest : public QObject {
    Q_OBJECT

private slots:
    void parsesStatusRequest();
    void parsesRunRequest();
    void rejectsUnknownCommand();
    void rejectsRunWithoutJobId();
    void rejectsEmptyRequest();
    void serialisesRequests();
    void statusJsonRoundTrip();
    void socketNameDependsOnDataRoot();
    void heartbeatRoundTrip();
    void heartbeatMissingFileMeansStopped();
};

void AgentProtocolTest::parsesStatusRequest() {
    const Result<Request> request =
        parseRequest(QJsonObject{{QStringLiteral("command"), QStringLiteral("status")}});
    QVERIFY(request);
    QCOMPARE(request.value().command, QStringLiteral("status"));
    QCOMPARE(request.value().jobId, qint64(0));
}

void AgentProtocolTest::parsesRunRequest() {
    QJsonObject object{{QStringLiteral("command"), QStringLiteral("run")},
                       {QStringLiteral("job_id"), 7},
                       {QStringLiteral("dry_run"), true},
                       {QStringLiteral("trigger"), QStringLiteral("cli")}};
    const Result<Request> request = parseRequest(object);
    QVERIFY(request);
    QCOMPARE(request.value().command, QStringLiteral("run"));
    QCOMPARE(request.value().jobId, qint64(7));
    QVERIFY(request.value().dryRun);
    QCOMPARE(request.value().trigger, QStringLiteral("cli"));
}

void AgentProtocolTest::rejectsUnknownCommand() {
    const Result<Request> request =
        parseRequest(QJsonObject{{QStringLiteral("command"), QStringLiteral("destroy")}});
    QVERIFY(!request);
    QCOMPARE(static_cast<int>(request.error().code()), static_cast<int>(ErrorCode::InvalidArgument));
    QVERIFY(request.error().message().find("unknown command") != std::string::npos);
}

void AgentProtocolTest::rejectsRunWithoutJobId() {
    const Result<Request> request =
        parseRequest(QJsonObject{{QStringLiteral("command"), QStringLiteral("run")}});
    QVERIFY(!request);
    QVERIFY(request.error().message().find("job id") != std::string::npos);
}

void AgentProtocolTest::rejectsEmptyRequest() {
    const Result<Request> request = parseRequest(QJsonObject{});
    QVERIFY(!request);
    QVERIFY(request.error().message().find("no command") != std::string::npos);
}

void AgentProtocolTest::serialisesRequests() {
    Request request;
    request.command = QStringLiteral("cancel");
    request.jobId = 12;
    const QJsonObject object = requestToJson(request);
    QCOMPARE(object.value(QStringLiteral("command")).toString(), QStringLiteral("cancel"));
    QCOMPARE(object.value(QStringLiteral("job_id")).toInt(), 12);

    // The serialised form must parse back identically.
    const Result<Request> roundTripped = parseRequest(object);
    QVERIFY(roundTripped);
    QCOMPARE(roundTripped.value().jobId, qint64(12));
}

void AgentProtocolTest::statusJsonRoundTrip() {
    AgentStatus status;
    status.running = true;
    status.pid = 1234;
    status.version = QStringLiteral("0.3.0");
    status.state = QStringLiteral("running");
    status.startedAtUtc = QDateTime::fromString(QStringLiteral("2026-03-10T02:00:00Z"), Qt::ISODate);
    status.updatedAtUtc = QDateTime::fromString(QStringLiteral("2026-03-10T02:05:00Z"), Qt::ISODate);
    status.runningJobId = 5;
    status.runningJobName = QStringLiteral("Documents");
    status.scheduledJobs = 3;
    status.nextRunUtc = QDateTime::fromString(QStringLiteral("2026-03-11T02:00:00Z"), Qt::ISODate);
    status.lastMessage = QStringLiteral("running 'Documents'");

    const QJsonObject object = statusToJson(status);
    const Result<AgentStatus> parsed = statusFromJson(object);
    QVERIFY(parsed);
    QCOMPARE(parsed.value().pid, qint64(1234));
    QCOMPARE(parsed.value().state, QStringLiteral("running"));
    QCOMPARE(parsed.value().runningJobId, qint64(5));
    QCOMPARE(parsed.value().runningJobName, QStringLiteral("Documents"));
    QCOMPARE(parsed.value().scheduledJobs, 3);
    QCOMPARE(parsed.value().nextRunUtc, status.nextRunUtc);
    QCOMPARE(parsed.value().lastMessage, status.lastMessage);
    QVERIFY(parsed.value().running);
}

void AgentProtocolTest::socketNameDependsOnDataRoot() {
    const QString previous = Paths::dataRootOverride();
    Paths::setDataRootOverride(QStringLiteral("/tmp/nesva-socket-a"));
    const QString first = socketName();
    Paths::setDataRootOverride(QStringLiteral("/tmp/nesva-socket-b"));
    const QString second = socketName();
    Paths::setDataRootOverride(previous);

    QVERIFY(first.startsWith(QStringLiteral("nesva-agent-")));
    QVERIFY(second.startsWith(QStringLiteral("nesva-agent-")));
    QVERIFY(first != second);
    // Stable for the same root.
    Paths::setDataRootOverride(QStringLiteral("/tmp/nesva-socket-a"));
    QCOMPARE(socketName(), first);
    Paths::setDataRootOverride(previous);
}

void AgentProtocolTest::heartbeatRoundTrip() {
    test::ScopedDataRoot dataRoot;
    QVERIFY(Paths::ensureBaseDirectories().ok());

    AgentStatus status;
    status.running = true;
    status.pid = 99;
    status.state = QStringLiteral("idle");
    status.startedAtUtc = QDateTime::currentDateTimeUtc();
    status.updatedAtUtc = QDateTime::currentDateTimeUtc();
    status.scheduledJobs = 2;

    const Result<QString> written = writeHeartbeat(status);
    QVERIFY(written);
    QVERIFY(QFile::exists(heartbeatPath()));

    const AgentStatus read = readHeartbeat();
    QCOMPARE(read.pid, qint64(99));
    QCOMPARE(read.state, QStringLiteral("idle"));
    QCOMPARE(read.scheduledJobs, 2);
    QVERIFY(read.running);
}

void AgentProtocolTest::heartbeatMissingFileMeansStopped() {
    test::ScopedDataRoot dataRoot;
    QVERIFY(Paths::ensureBaseDirectories().ok());
    const AgentStatus status = readHeartbeat();
    QVERIFY(!status.running);
    QCOMPARE(status.state, QStringLiteral("stopped"));
}

QTEST_GUILESS_MAIN(AgentProtocolTest)
#include "tst_agent_protocol.moc"
