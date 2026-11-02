// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include "nesva/backup/restic.hpp"
#include "nesva/core/external_tool.hpp"
#include "nesva/core/paths.hpp"
#include "nesva/db/database.hpp"
#include "nesva/jobs/backup_runner.hpp"
#include "nesva/jobs/job_store.hpp"
#include "support/test_fixtures.hpp"

using namespace nesva;

class BackupRunnerTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void runsBackupAndRecordsSnapshot();
    void secondRunIsIncremental();
    void cancelledBackupIsRecorded();
    void dryRunWritesNothing();
    void restoresSnapshotWithoutOverwriting();
    void passwordRoundTripThroughFile();

private:
    bool resticAvailable() const {
        return !resolveExecutable(QStringLiteral("restic")).isEmpty();
    }

    jobs::PasswordOptions filePasswordOptions() const {
        jobs::PasswordOptions options;
        options.source = jobs::PasswordSource::File;
        options.filePath = passwordFile_;
        return options;
    }

    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<db::Database> database_;
    std::unique_ptr<jobs::JobStore> store_;
    qint64 jobId_ = 0;
    QString passwordFile_;
    QString sourceDir_;
    QString repositoryDir_;
};

void BackupRunnerTest::init() {
    if (!resticAvailable()) {
        QSKIP("restic is not installed");
    }
    directory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(directory_->isValid());
    Paths::setDataRootOverride(directory_->path());
    QVERIFY(Paths::ensureBaseDirectories().ok());

    passwordFile_ = directory_->filePath(QStringLiteral("repo-password"));
    QVERIFY(test::writeFile(passwordFile_, QByteArray("unit-runner-password\n")));
    QFile::setPermissions(passwordFile_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    sourceDir_ = directory_->filePath(QStringLiteral("source"));
    repositoryDir_ = directory_->filePath(QStringLiteral("repo"));
    QVERIFY(QDir().mkpath(sourceDir_));
    QVERIFY(test::writeFile(QDir(sourceDir_).absoluteFilePath(QStringLiteral("a.txt")), "alpha"));
    QVERIFY(test::writeFile(
        QDir(sourceDir_).absoluteFilePath(QStringLiteral("docs/b.txt")), "bravo"));

    database_ = std::make_unique<db::Database>();
    QVERIFY(database_->open(directory_->filePath(QStringLiteral("nesva.db"))).ok());
    QVERIFY(database_->migrate().ok());
    store_ = std::make_unique<jobs::JobStore>(*database_);

    jobs::ArchiveJob job;
    job.name = QStringLiteral("Restic unit job");
    job.mode = QStringLiteral("backup");
    job.destinationPath = repositoryDir_;
    job.sources = {{0, sourceDir_, QStringLiteral("source")}};
    job.retention.keepLast = 2;
    const Result<qint64> created = store_->createJob(job);
    QVERIFY(created);
    jobId_ = created.value();

    // Initialise the repository with the same password contract the runner uses.
    const Result<QString> passwordFile = backup::createPasswordFile(QByteArray("unit-runner-password"));
    QVERIFY(passwordFile);
    backup::ResticRepository repository(QStringLiteral("restic"), repositoryDir_, passwordFile.value());
    QVERIFY2(repository.init().ok(), "repository init");
    QFile::remove(passwordFile.value());
}

void BackupRunnerTest::cleanup() {
    store_.reset();
    database_.reset();
    Paths::setDataRootOverride(QString());
    directory_.reset();
}

void BackupRunnerTest::runsBackupAndRecordsSnapshot() {
    const CancellationToken cancellation;
    jobs::BackupRunner runner(*database_);
    const jobs::RunReport report = runner.run(store_->getJob(jobId_).value(), {}, cancellation,
                                              QStringLiteral("test"), QStringLiteral("restic"),
                                              filePasswordOptions());

    QCOMPARE(report.status, QStringLiteral("succeeded"));
    QVERIFY(report.runId > 0);
    QCOMPARE(report.counters.filesTransferred, qint64(2));

    const Result<jobs::RunSummary> summary = store_->getRun(report.runId);
    QVERIFY(summary);
    QCOMPARE(summary.value().status, QStringLiteral("succeeded"));

    const Result<QVector<jobs::JobStore::SnapshotRecord>> snapshots = store_->listSnapshots(jobId_);
    QVERIFY(snapshots);
    QCOMPARE(snapshots.value().size(), 1);
    QVERIFY(!snapshots.value().constFirst().snapshotId.isEmpty());
}

void BackupRunnerTest::secondRunIsIncremental() {
    const CancellationToken cancellation;
    jobs::BackupRunner runner(*database_);
    QVERIFY(runner.run(store_->getJob(jobId_).value(), {}, cancellation, QStringLiteral("test"),
                       QStringLiteral("restic"), filePasswordOptions())
                .succeeded());

    const jobs::RunReport second = runner.run(store_->getJob(jobId_).value(), {}, cancellation,
                                              QStringLiteral("test"), QStringLiteral("restic"),
                                              filePasswordOptions());
    QCOMPARE(second.status, QStringLiteral("succeeded"));
    QCOMPARE(second.counters.filesSkipped, qint64(2));
    QCOMPARE(second.counters.filesTransferred, qint64(0));

    const Result<QVector<jobs::JobStore::SnapshotRecord>> snapshots = store_->listSnapshots(jobId_);
    QVERIFY(snapshots);
    QCOMPARE(snapshots.value().size(), 2);
}

void BackupRunnerTest::cancelledBackupIsRecorded() {
    CancellationToken cancellation;
    cancellation.cancel();
    jobs::BackupRunner runner(*database_);
    const jobs::RunReport report = runner.run(store_->getJob(jobId_).value(), {}, cancellation,
                                              QStringLiteral("test"), QStringLiteral("restic"),
                                              filePasswordOptions());
    QCOMPARE(report.status, QStringLiteral("cancelled"));
    const Result<jobs::RunSummary> summary = store_->getRun(report.runId);
    QVERIFY(summary);
    QCOMPARE(summary.value().status, QStringLiteral("cancelled"));
}

void BackupRunnerTest::dryRunWritesNothing() {
    const CancellationToken cancellation;
    jobs::BackupRunner runner(*database_);
    jobs::RunOptions options;
    options.dryRun = true;
    const jobs::RunReport report = runner.run(store_->getJob(jobId_).value(), options, cancellation,
                                              QStringLiteral("test"), QStringLiteral("restic"),
                                              filePasswordOptions());
    QVERIFY(report.dryRun);
    QCOMPARE(report.status, QStringLiteral("succeeded"));

    const Result<QVector<jobs::JobStore::SnapshotRecord>> snapshots = store_->listSnapshots(jobId_);
    QVERIFY(snapshots);
    QCOMPARE(snapshots.value().size(), 0);
    const Result<QVector<jobs::RunSummary>> runs = store_->listRuns(10, jobId_);
    QVERIFY(runs);
    QCOMPARE(runs.value().size(), 0);
}

void BackupRunnerTest::restoresSnapshotWithoutOverwriting() {
    const CancellationToken cancellation;
    jobs::BackupRunner runner(*database_);
    QVERIFY(runner.run(store_->getJob(jobId_).value(), {}, cancellation, QStringLiteral("test"),
                       QStringLiteral("restic"), filePasswordOptions())
                .succeeded());

    const QString target = directory_->filePath(QStringLiteral("restore"));
    QVERIFY(QDir().mkpath(target));
    // Pre-existing file that must not be overwritten by the "skip" policy.
    QVERIFY(test::writeFile(QDir(target).absoluteFilePath(QStringLiteral("a.txt")), "existing"));

    jobs::BackupRestorer::Request request;
    request.job = store_->getJob(jobId_).value();
    request.resticExecutable = QStringLiteral("restic");
    request.snapshotId = QStringLiteral("latest");
    request.destinationDirectory = target;
    request.policy = jobs::ConflictPolicy::Skip;
    request.passwordOptions = filePasswordOptions();

    jobs::BackupRestorer restorer(*database_);
    const jobs::BackupRestorer::Report report = restorer.restore(request, cancellation);

    QCOMPARE(report.status, QStringLiteral("succeeded"));
    QCOMPARE(report.skipped, 1);
    QCOMPARE(report.restored, 1);
    QCOMPARE(test::readFile(QDir(target).absoluteFilePath(QStringLiteral("a.txt"))),
             QByteArray("existing"));
    QCOMPARE(test::readFile(QDir(target).absoluteFilePath(QStringLiteral("docs/b.txt"))),
             QByteArray("bravo"));

    // Ask must be refused before anything is written.
    jobs::BackupRestorer::Request askRequest = request;
    askRequest.policy = jobs::ConflictPolicy::Ask;
    const jobs::BackupRestorer::Report refused = restorer.restore(askRequest, cancellation);
    QVERIFY(!refused.succeeded());
    QCOMPARE(refused.restored, 0);
}

void BackupRunnerTest::passwordRoundTripThroughFile() {
    const QString repository = directory_->filePath(QStringLiteral("another-repo"));
    jobs::PasswordOptions options = filePasswordOptions();
    QVERIFY(jobs::storeRepositoryPassword(repository, options, QByteArray("secret-123")).ok());

    const Result<QByteArray> loaded = jobs::loadRepositoryPassword(repository, options);
    QVERIFY(loaded);
    QCOMPARE(loaded.value(), QByteArray("secret-123"));

    // The account derivation must be stable and must not leak the path itself.
    const QString account = jobs::repositoryAccount(repository);
    QVERIFY(account.startsWith(QStringLiteral("restic-")));
    QCOMPARE(account, jobs::repositoryAccount(repository));
    QVERIFY(!account.contains(QStringLiteral("/")));
}

QTEST_GUILESS_MAIN(BackupRunnerTest)
#include "tst_backup_runner.moc"
