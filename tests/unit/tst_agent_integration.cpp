// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

// Integration test: starts the real nesva-agent executable against a temporary
// data directory and talks to it over its local socket, exactly like the
// desktop application and nesva-cli do.

#include <QtTest/QtTest>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>

#include "nesva/agent/client.hpp"
#include "nesva/agent/protocol.hpp"
#include "nesva/core/paths.hpp"
#include "nesva/db/database.hpp"
#include "nesva/jobs/job_store.hpp"
#include "support/test_fixtures.hpp"

using namespace nesva;

class AgentIntegrationTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void runsScheduledJobOnStartup();
    void reportsStatusOverIpc();
    void acceptsRunCommand();
    void rejectsCancelWhenIdle();
    void shutsDownGracefully();

private:
    bool startAgent();
    void stopAgent();

    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<QProcess> agent_;
    QString agentPath_;
    QString sourceDir_;
    QString destinationDir_;
    qint64 jobId_ = 0;
};

void AgentIntegrationTest::init() {
    agentPath_ = qEnvironmentVariable("NESVA_AGENT_PATH");
    QVERIFY2(!agentPath_.isEmpty(), "NESVA_AGENT_PATH must point at the built nesva-agent");
    QVERIFY2(QFileInfo::exists(agentPath_), qPrintable(agentPath_));

    directory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(directory_->isValid());
    Paths::setDataRootOverride(directory_->path());
    QVERIFY(Paths::ensureBaseDirectories().ok());

    sourceDir_ = directory_->filePath(QStringLiteral("source"));
    destinationDir_ = directory_->filePath(QStringLiteral("destination"));
    QVERIFY(QDir().mkpath(sourceDir_));
    QVERIFY(QDir().mkpath(destinationDir_));
    QVERIFY(test::writeFile(QDir(sourceDir_).absoluteFilePath(QStringLiteral("a.txt")), "alpha"));
    QVERIFY(test::writeFile(QDir(sourceDir_).absoluteFilePath(QStringLiteral("docs/b.txt")), "bravo"));

    db::Database database;
    QVERIFY(database.open(Paths::databasePath()).ok());
    QVERIFY(database.migrate().ok());
    jobs::JobStore store(database);

    jobs::ArchiveJob job;
    job.name = QStringLiteral("Agent job");
    job.mode = QStringLiteral("archive");
    job.destinationPath = destinationDir_;
    job.sources = {{0, sourceDir_, QStringLiteral("source")}};
    const Result<qint64> created = store.createJob(job);
    QVERIFY(created);
    jobId_ = created.value();

    sched::ScheduleSpec spec;
    spec.kind = sched::TriggerKind::OnStartup;
    spec.timezone = QByteArrayLiteral("UTC");
    QVERIFY(store.upsertSchedule(jobId_, spec).ok());
}

void AgentIntegrationTest::cleanup() {
    stopAgent();
    Paths::setDataRootOverride(QString());
    directory_.reset();
}

bool AgentIntegrationTest::startAgent() {
    agent_ = std::make_unique<QProcess>();
    agent_->setProgram(agentPath_);
    agent_->setArguments({QStringLiteral("--data-dir"), directory_->path(),
                          QStringLiteral("--tick"), QStringLiteral("1")});
    agent_->setProcessChannelMode(QProcess::SeparateChannels);
    agent_->start();
    if (!agent_->waitForStarted(10000)) {
        qWarning("agent failed to start: %s", qPrintable(agent_->errorString()));
        return false;
    }
    // Wait until the socket answers.
    for (int attempt = 0; attempt < 100; ++attempt) {
        agent::AgentClient client;
        if (client.isReachable(100)) {
            return true;
        }
        if (agent_->state() == QProcess::NotRunning) {
            qWarning("agent exited early (exit code %d):\n%s",
                     agent_->exitCode(),
                     agent_->readAllStandardError().constData());
            return false;
        }
        QThread::msleep(100);
    }
    qWarning("agent did not answer on socket '%s'\nstdout:\n%s\nstderr:\n%s",
             qPrintable(agent::socketName()),
             agent_->readAllStandardOutput().constData(),
             agent_->readAllStandardError().constData());
    return false;
}

void AgentIntegrationTest::stopAgent() {
    if (agent_) {
        if (agent_->state() != QProcess::NotRunning) {
            agent_->terminate();
            if (!agent_->waitForFinished(10000)) {
                agent_->kill();
                agent_->waitForFinished(5000);
            }
        }
        agent_.reset();
    }
}

void AgentIntegrationTest::runsScheduledJobOnStartup() {
    QVERIFY(startAgent());

    const QString archived = QDir(destinationDir_).absoluteFilePath(QStringLiteral("source/a.txt"));
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(archived), 30000);
    QCOMPARE(test::readFile(archived), QByteArray("alpha"));
    QVERIFY(QFile::exists(
        QDir(destinationDir_).absoluteFilePath(QStringLiteral("source/docs/b.txt"))));

    // Source files are untouched.
    QCOMPARE(test::readFile(QDir(sourceDir_).absoluteFilePath(QStringLiteral("a.txt"))),
             QByteArray("alpha"));
}

void AgentIntegrationTest::reportsStatusOverIpc() {
    QVERIFY(startAgent());
    agent::AgentClient client;
    const Result<agent::AgentStatus> status = client.status(3000);
    QVERIFY2(status.hasValue(),
             status.hasValue() ? "" : status.error().message().c_str());
    QVERIFY(status.value().running);
    QVERIFY(status.value().pid > 0);
    QVERIFY(status.value().scheduledJobs >= 1);
    QVERIFY(status.value().version.contains(QLatin1Char('.')));
}

void AgentIntegrationTest::acceptsRunCommand() {
    QVERIFY(startAgent());

    agent::AgentClient client;
    // Wait until the startup run has finished so the agent is idle.
    QTRY_VERIFY_WITH_TIMEOUT(client.status(1000).valueOr(agent::AgentStatus{}).state ==
                                 QStringLiteral("idle"),
                             30000);

    const Result<QString> accepted = client.runJob(jobId_, false, QStringLiteral("integration-test"));
    QVERIFY2(accepted.hasValue(),
             accepted.hasValue() ? "" : accepted.error().message().c_str());

    // The run appears in history with the requested trigger.
    db::Database database;
    QVERIFY(database.open(Paths::databasePath()).ok());
    jobs::JobStore store(database);
    QTRY_VERIFY_WITH_TIMEOUT(store.listRuns(5, jobId_).valueOr({}).size() >= 2, 30000);
    bool foundRequestedTrigger = false;
    for (const jobs::RunSummary& run : store.listRuns(10, jobId_).valueOr({})) {
        if (run.trigger == QStringLiteral("integration-test")) {
            foundRequestedTrigger = true;
        }
    }
    QVERIFY(foundRequestedTrigger);
}

void AgentIntegrationTest::rejectsCancelWhenIdle() {
    QVERIFY(startAgent());
    agent::AgentClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.status(1000).valueOr(agent::AgentStatus{}).state ==
                                 QStringLiteral("idle"),
                             30000);

    const Status cancelled = client.cancelJob(jobId_);
    QVERIFY(!cancelled.ok());
    QVERIFY(cancelled.error().message().find("not running") != std::string::npos);
}

void AgentIntegrationTest::shutsDownGracefully() {
    QVERIFY(startAgent());
    agent::AgentClient client;
    QVERIFY(client.shutdown().ok());
    QVERIFY(agent_->waitForFinished(20000));
    QCOMPARE(agent_->exitCode(), 0);

    // The heartbeat records a clean stop.
    const agent::AgentStatus heartbeat = agent::AgentClient::lastKnownStatus();
    QVERIFY(!heartbeat.running);
    QCOMPARE(heartbeat.state, QStringLiteral("stopped"));
}

QTEST_GUILESS_MAIN(AgentIntegrationTest)
#include "tst_agent_integration.moc"
