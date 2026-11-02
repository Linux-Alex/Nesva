// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtSql/QSqlQuery>

#include <utility>

#include "nesva/core/cancellation.hpp"
#include "nesva/jobs/archive_runner.hpp"
#include "support/test_archive_fixture.hpp"
#include "support/test_fixtures.hpp"

using namespace nesva;
using nesva::jobs::ArchiveRunner;
using nesva::jobs::RunOptions;
using nesva::jobs::RunReport;

class ArchiveRunnerTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void firstRunArchivesEveryFile();
    void secondRunSkipsUnchangedFiles();
    void modifiedFileIsTransferredAgain();
    void dryRunWritesNothing();
    void destinationInsideSourceIsNotRecursivelyArchived();
    void exclusionsAreHonoured();
    void duplicateRunIsRejected();
    void cancellationMarksRunCancelled();
    void failedFileIsRecordedAndRunContinues();
    void missingSourceFailsTheRun();
    void recordsEventsAndCounters();

private:
    std::unique_ptr<test::ArchiveFixture> fixture_;

    RunReport runArchive(const RunOptions& options = {}, const CancellationToken& cancellation = {}) {
        ArchiveRunner runner(*fixture_->database);
        return runner.run(fixture_->job(), std::nullopt, options, cancellation);
    }
};

void ArchiveRunnerTest::init() {
    fixture_ = std::make_unique<test::ArchiveFixture>();
    QVERIFY(fixture_->setup());
    QVERIFY(fixture_->writeSource(QStringLiteral("a.txt"), "alpha"));
    QVERIFY(fixture_->writeSource(QStringLiteral("docs/b.txt"), "bravo"));
    QVERIFY(fixture_->writeSource(QStringLiteral("docs/nested/c.txt"), "charlie"));
}

void ArchiveRunnerTest::cleanup() {
    fixture_.reset();
}

void ArchiveRunnerTest::firstRunArchivesEveryFile() {
    const RunReport report = runArchive();
    QCOMPARE(report.status, QStringLiteral("succeeded"));
    QCOMPARE(report.uploads, 3);
    QCOMPARE(report.counters.filesTotal, qint64(3));
    QCOMPARE(report.counters.filesTransferred, qint64(3));
    QCOMPARE(report.counters.filesSkipped, qint64(0));
    QCOMPARE(report.failures, 0);
    QVERIFY(report.runId > 0);

    QCOMPARE(test::readFile(fixture_->destinationPath(QStringLiteral("a.txt"))), QByteArray("alpha"));
    QCOMPARE(test::readFile(fixture_->destinationPath(QStringLiteral("docs/b.txt"))),
             QByteArray("bravo"));
    QCOMPARE(test::readFile(fixture_->destinationPath(QStringLiteral("docs/nested/c.txt"))),
             QByteArray("charlie"));

    // Source files are untouched: archive mode never deletes or rewrites.
    QCOMPARE(test::readFile(fixture_->sourcePath(QStringLiteral("a.txt"))), QByteArray("alpha"));

    const Result<QVector<jobs::ManifestRecord>> manifest = fixture_->store->latestManifest(fixture_->jobId);
    QVERIFY(manifest);
    QCOMPARE(manifest.value().size(), 3);
    for (const jobs::ManifestRecord& record : manifest.value()) {
        QCOMPARE(record.state, QStringLiteral("verified"));
        QVERIFY(!record.sha256.isEmpty());
    }
}

void ArchiveRunnerTest::secondRunSkipsUnchangedFiles() {
    QCOMPARE(runArchive().status, QStringLiteral("succeeded"));

    const RunReport second = runArchive();
    QCOMPARE(second.status, QStringLiteral("succeeded"));
    QCOMPARE(second.uploads, 0);
    QCOMPARE(second.counters.filesTransferred, qint64(0));
    QCOMPARE(second.counters.filesSkipped, qint64(3));

    const Result<QVector<jobs::ManifestRecord>> manifest = fixture_->store->latestManifest(fixture_->jobId);
    QVERIFY(manifest);
    for (const jobs::ManifestRecord& record : manifest.value()) {
        QCOMPARE(record.state, QStringLiteral("skipped"));
        QVERIFY(record.skipReason.contains(QStringLiteral("unchanged")));
    }
}

void ArchiveRunnerTest::modifiedFileIsTransferredAgain() {
    QCOMPARE(runArchive().status, QStringLiteral("succeeded"));

    // New content and a newer timestamp.
    QVERIFY(test::writeFile(fixture_->sourcePath(QStringLiteral("docs/b.txt")), "bravo v2 (longer)"));
    const QDateTime future = QDateTime::currentDateTimeUtc().addSecs(5);
    QFile touched(fixture_->sourcePath(QStringLiteral("docs/b.txt")));
    QVERIFY(touched.open(QIODevice::ReadWrite));
    QVERIFY(touched.setFileTime(future, QFileDevice::FileModificationTime));
    touched.close();

    const RunReport second = runArchive();
    QCOMPARE(second.status, QStringLiteral("succeeded"));
    QCOMPARE(second.counters.filesTransferred, qint64(1));
    QCOMPARE(second.counters.filesSkipped, qint64(2));
    QCOMPARE(test::readFile(fixture_->destinationPath(QStringLiteral("docs/b.txt"))),
             QByteArray("bravo v2 (longer)"));
}

void ArchiveRunnerTest::dryRunWritesNothing() {
    RunOptions options;
    options.dryRun = true;
    const RunReport report = runArchive(options);

    QVERIFY(report.dryRun);
    QCOMPARE(report.status, QStringLiteral("succeeded"));
    QCOMPARE(report.uploads, 3);
    QCOMPARE(report.skipped, 0);

    // Nothing on disk, nothing in history.
    QVERIFY(!QFileInfo::exists(fixture_->destinationPath(QStringLiteral("a.txt"))));
    Result<QSqlQuery> runs =
        fixture_->database->select(QStringLiteral("SELECT COUNT(*) FROM job_runs"));
    QVERIFY(runs);
    QSqlQuery rows = std::move(runs).take();
    QVERIFY(rows.next());
    QCOMPARE(rows.value(0).toInt(), 0);
}

void ArchiveRunnerTest::destinationInsideSourceIsNotRecursivelyArchived() {
    // A destination inside the source tree must never be scanned, otherwise a
    // job would archive its own output forever.
    jobs::ArchiveJob job = fixture_->job();
    job.destinationPath = QDir(fixture_->sourceDir).absoluteFilePath(QStringLiteral("archive"));
    QVERIFY(fixture_->store->updateJob(job).ok());

    ArchiveRunner runner(*fixture_->database);
    const RunReport report = runner.run(fixture_->job(), std::nullopt, {}, {});
    QCOMPARE(report.status, QStringLiteral("succeeded"));
    QCOMPARE(report.counters.filesTotal, qint64(3));

    const QString archivedTree = QDir(job.destinationPath).absoluteFilePath(QStringLiteral("source"));
    QVERIFY(QFileInfo::exists(QDir(archivedTree).absoluteFilePath(QStringLiteral("a.txt"))));
    // No archive-of-the-archive directory appeared.
    QVERIFY(!QFileInfo::exists(QDir(archivedTree).absoluteFilePath(QStringLiteral("archive"))));

    // A second run still sees exactly three files.
    const RunReport second = runner.run(fixture_->job(), std::nullopt, {}, {});
    QCOMPARE(second.counters.filesTotal, qint64(3));
    QCOMPARE(second.counters.filesSkipped, qint64(3));
}

void ArchiveRunnerTest::exclusionsAreHonoured() {
    QVERIFY(test::writeFile(fixture_->sourcePath(QStringLiteral("debug.tmp")), "tmp"));
    jobs::ArchiveJob job = fixture_->job();
    job.exclusions = {{QStringLiteral("*.tmp"), scan::PatternKind::Glob}};
    QVERIFY(fixture_->store->updateJob(job).ok());

    ArchiveRunner runner(*fixture_->database);
    const RunReport report = runner.run(fixture_->job(), std::nullopt, {}, {});
    QCOMPARE(report.status, QStringLiteral("succeeded"));
    QCOMPARE(report.counters.filesTotal, qint64(3));
    QVERIFY(!QFileInfo::exists(fixture_->destinationPath(QStringLiteral("debug.tmp"))));
}

void ArchiveRunnerTest::duplicateRunIsRejected() {
    const Result<qint64> running = fixture_->store->beginRun(fixture_->jobId, QStringLiteral("manual"));
    QVERIFY(running);

    const RunReport report = runArchive();
    QCOMPARE(report.status, QStringLiteral("failed"));
    QVERIFY(!report.errors.isEmpty());
    QVERIFY(report.errors.constFirst().contains(QStringLiteral("already in progress")));
}

void ArchiveRunnerTest::cancellationMarksRunCancelled() {
    CancellationToken cancellation;
    cancellation.cancel();

    const RunReport report = runArchive({}, cancellation);
    QCOMPARE(report.status, QStringLiteral("cancelled"));

    const Result<jobs::RunSummary> summary = fixture_->store->getRun(report.runId);
    QVERIFY(summary);
    QCOMPARE(summary.value().status, QStringLiteral("cancelled"));
    QVERIFY(!QFileInfo::exists(fixture_->destinationPath(QStringLiteral("a.txt"))));
}

void ArchiveRunnerTest::failedFileIsRecordedAndRunContinues() {
#ifndef Q_OS_UNIX
    QSKIP("permission based failure injection requires POSIX permissions");
#else
    if (::getuid() == 0) {
        QSKIP("running as root: file permissions do not prevent reads");
    }
    const QString blocked = fixture_->sourcePath(QStringLiteral("docs/b.txt"));
    QVERIFY(test::writeFile(blocked, "bravo"));
    QFile file(blocked);
    QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    // Remove read permission for the owner as well.
    QVERIFY(QFile::setPermissions(blocked, QFileDevice::WriteOwner));

    const RunReport report = runArchive();
    QVERIFY(QFile::setPermissions(blocked, QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    QCOMPARE(report.status, QStringLiteral("failed"));
    QCOMPARE(report.counters.filesFailed, qint64(1));
    QVERIFY(report.counters.filesTransferred >= qint64(1));
    QVERIFY(!report.errors.isEmpty());

    const Result<QVector<jobs::ManifestRecord>> manifest =
        fixture_->store->latestManifest(fixture_->jobId);
    QVERIFY(manifest);
    bool failedEntry = false;
    for (const jobs::ManifestRecord& record : manifest.value()) {
        if (record.state == QStringLiteral("failed")) {
            failedEntry = true;
        }
    }
    QVERIFY(failedEntry);
#endif
}

void ArchiveRunnerTest::missingSourceFailsTheRun() {
    jobs::ArchiveJob job = fixture_->job();
    job.sources = {{0, fixture_->root->filePath(QStringLiteral("nope")), QStringLiteral("nope")}};
    QVERIFY(fixture_->store->updateJob(job).ok());

    ArchiveRunner runner(*fixture_->database);
    const RunReport report = runner.run(fixture_->job(), std::nullopt, {}, {});
    QCOMPARE(report.status, QStringLiteral("failed"));
    QVERIFY(!report.errors.isEmpty());

    const Result<jobs::RunSummary> summary = fixture_->store->getRun(report.runId);
    QVERIFY(summary);
    QCOMPARE(summary.value().status, QStringLiteral("failed"));
}

void ArchiveRunnerTest::recordsEventsAndCounters() {
    const RunReport report = runArchive();
    QVERIFY(report.runId > 0);

    Result<QSqlQuery> events = fixture_->database->select(
        QStringLiteral("SELECT message, level FROM job_events WHERE job_run_id = ? ORDER BY id"),
        {QVariant(report.runId)});
    QVERIFY(events);
    QSqlQuery rows = std::move(events).take();
    QVERIFY(rows.next());
    QVERIFY(rows.value(0).toString().contains(QStringLiteral("started")));
    QVERIFY(rows.next());
    QVERIFY(rows.value(0).toString().contains(QStringLiteral("transferred")));
    QCOMPARE(rows.value(1).toString(), QStringLiteral("info"));

    const Result<jobs::RunSummary> summary = fixture_->store->getRun(report.runId);
    QVERIFY(summary);
    QCOMPARE(summary.value().counters.filesTotal, qint64(3));
    QCOMPARE(summary.value().counters.bytesTransferred,
             QFileInfo(fixture_->sourcePath(QStringLiteral("a.txt"))).size() +
                 QFileInfo(fixture_->sourcePath(QStringLiteral("docs/b.txt"))).size() +
                 QFileInfo(fixture_->sourcePath(QStringLiteral("docs/nested/c.txt"))).size());
}

QTEST_GUILESS_MAIN(ArchiveRunnerTest)
#include "tst_archive_runner.moc"
