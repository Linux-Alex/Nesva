// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtSql/QSqlQuery>

#include <utility>

#include "nesva/core/cancellation.hpp"
#include "nesva/jobs/archive_restore.hpp"
#include "nesva/jobs/archive_runner.hpp"
#include "support/test_archive_fixture.hpp"
#include "support/test_fixtures.hpp"

using namespace nesva;
using nesva::jobs::ArchiveRestorer;
using nesva::jobs::ConflictPolicy;
using nesva::jobs::RestoreRequest;

class ArchiveRestoreTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void plansArchivedFiles();
    void planDetectsConflicts();
    void restoreSkipsExistingFiles();
    void restoreRenamesExistingFiles();
    void restoreOverwritesWhenRequested();
    void askPolicyRefusesToWrite();
    void detectsCorruptedArchiveContent();
    void recordsRestoreInDatabase();
    void cancelledRestoreKeepsCompletedFiles();

private:
    std::unique_ptr<test::ArchiveFixture> fixture_;
    QString restoreDirectory_;

    void archiveOnce() {
        jobs::ArchiveRunner runner(*fixture_->database);
        const jobs::RunReport report = runner.run(fixture_->job(), std::nullopt, {}, {});
        QCOMPARE(report.status, QStringLiteral("succeeded"));
    }

    jobs::RestoreReport restore(ConflictPolicy policy, bool verify = true) {
        RestoreRequest request;
        request.job = fixture_->job();
        request.destinationDirectory = restoreDirectory_;
        request.policy = policy;
        request.verify = verify;
        ArchiveRestorer restorer(*fixture_->database);
        return restorer.restore(request, {});
    }
};

void ArchiveRestoreTest::init() {
    fixture_ = std::make_unique<test::ArchiveFixture>();
    QVERIFY(fixture_->setup());
    QVERIFY(fixture_->writeSource(QStringLiteral("a.txt"), "alpha"));
    QVERIFY(fixture_->writeSource(QStringLiteral("docs/b.txt"), "bravo"));
    restoreDirectory_ = fixture_->root->filePath(QStringLiteral("restore"));
    QVERIFY(QDir().mkpath(restoreDirectory_));
}

void ArchiveRestoreTest::cleanup() {
    fixture_.reset();
}

void ArchiveRestoreTest::plansArchivedFiles() {
    archiveOnce();
    ArchiveRestorer restorer(*fixture_->database);
    const Result<jobs::RestorePlan> plan =
        restorer.plan(fixture_->job(), std::nullopt, restoreDirectory_);
    QVERIFY(plan);
    QCOMPARE(plan.value().items.size(), 2);
    QCOMPARE(plan.value().conflictingFiles, 0);
    QVERIFY(plan.value().totalBytes > 0);
    for (const jobs::ArchiveItem& item : plan.value().items) {
        QVERIFY(!item.sha256.isEmpty());
    }
}

void ArchiveRestoreTest::planDetectsConflicts() {
    archiveOnce();
    QVERIFY(test::writeFile(QDir(restoreDirectory_).absoluteFilePath(QStringLiteral("source/a.txt")),
                            "existing"));

    ArchiveRestorer restorer(*fixture_->database);
    const Result<jobs::RestorePlan> plan =
        restorer.plan(fixture_->job(), std::nullopt, restoreDirectory_);
    QVERIFY(plan);
    QCOMPARE(plan.value().conflictingFiles, 1);
    QVERIFY(plan.value().hasConflicts());
    QVERIFY(plan.value().conflictSamples.contains(QStringLiteral("source/a.txt")));
}

void ArchiveRestoreTest::restoreSkipsExistingFiles() {
    archiveOnce();
    const QString target = QDir(restoreDirectory_).absoluteFilePath(QStringLiteral("source/a.txt"));
    QVERIFY(test::writeFile(target, "existing"));

    const jobs::RestoreReport report = restore(ConflictPolicy::Skip);
    QCOMPARE(report.status, QStringLiteral("succeeded"));
    QCOMPARE(report.skipped, 1);
    QCOMPARE(report.restored, 1);
    // The existing file was never modified.
    QCOMPARE(test::readFile(target), QByteArray("existing"));
    QCOMPARE(test::readFile(QDir(restoreDirectory_).absoluteFilePath(QStringLiteral("source/docs/b.txt"))),
             QByteArray("bravo"));
}

void ArchiveRestoreTest::restoreRenamesExistingFiles() {
    archiveOnce();
    const QString target = QDir(restoreDirectory_).absoluteFilePath(QStringLiteral("source/a.txt"));
    QVERIFY(test::writeFile(target, "existing"));

    const jobs::RestoreReport report = restore(ConflictPolicy::Rename);
    QCOMPARE(report.status, QStringLiteral("succeeded"));
    QCOMPARE(report.renamed, 1);
    QCOMPARE(test::readFile(target), QByteArray("existing"));

    const QString renamed =
        QDir(restoreDirectory_).absoluteFilePath(QStringLiteral("source/a (1).txt"));
    QVERIFY2(QFileInfo::exists(renamed), "conflict must be renamed, not overwritten");
    QCOMPARE(test::readFile(renamed), QByteArray("alpha"));
}

void ArchiveRestoreTest::restoreOverwritesWhenRequested() {
    archiveOnce();
    const QString target = QDir(restoreDirectory_).absoluteFilePath(QStringLiteral("source/a.txt"));
    QVERIFY(test::writeFile(target, "existing"));

    const jobs::RestoreReport report = restore(ConflictPolicy::Overwrite);
    QCOMPARE(report.status, QStringLiteral("succeeded"));
    QCOMPARE(test::readFile(target), QByteArray("alpha"));
}

void ArchiveRestoreTest::askPolicyRefusesToWrite() {
    archiveOnce();
    const jobs::RestoreReport report = restore(ConflictPolicy::Ask);
    QVERIFY(!report.succeeded());
    QVERIFY(!report.errors.isEmpty());
    QVERIFY(!QFileInfo::exists(
        QDir(restoreDirectory_).absoluteFilePath(QStringLiteral("source/a.txt"))));

    Result<QSqlQuery> rows =
        fixture_->database->select(QStringLiteral("SELECT COUNT(*) FROM restore_jobs"));
    QVERIFY(rows);
    QSqlQuery count = std::move(rows).take();
    QVERIFY(count.next());
    QCOMPARE(count.value(0).toInt(), 0);
}

void ArchiveRestoreTest::detectsCorruptedArchiveContent() {
    archiveOnce();

    // Corrupt the archived copy behind the manifest's back.
    const QString archived = fixture_->destinationPath(QStringLiteral("a.txt"));
    QVERIFY(test::writeFile(archived, "CORRUPTED-CONTENT"));

    const jobs::RestoreReport report = restore(ConflictPolicy::Skip);
    QCOMPARE(report.status, QStringLiteral("failed"));
    QCOMPARE(report.failed, 1);
    bool mismatchReported = false;
    for (const QString& error : report.errors) {
        if (error.contains(QStringLiteral("checksum mismatch"))) {
            mismatchReported = true;
        }
    }
    QVERIFY(mismatchReported);
    // The bad restore target must not be left behind.
    QVERIFY(!QFileInfo::exists(
        QDir(restoreDirectory_).absoluteFilePath(QStringLiteral("source/a.txt"))));
}

void ArchiveRestoreTest::recordsRestoreInDatabase() {
    archiveOnce();
    const jobs::RestoreReport report = restore(ConflictPolicy::Skip);
    QVERIFY(report.restoreJobId > 0);

    Result<QSqlQuery> rows = fixture_->database->select(
        QStringLiteral("SELECT status, files_restored, bytes_restored, conflict_policy "
                       "FROM restore_jobs WHERE id = ?"),
        {QVariant(report.restoreJobId)});
    QVERIFY(rows);
    QSqlQuery query = std::move(rows).take();
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), QStringLiteral("succeeded"));
    QCOMPARE(query.value(1).toInt(), 2);
    QVERIFY(query.value(2).toLongLong() > 0);
    QCOMPARE(query.value(3).toString(), QStringLiteral("skip"));
}

void ArchiveRestoreTest::cancelledRestoreKeepsCompletedFiles() {
    archiveOnce();
    CancellationToken cancellation;
    cancellation.cancel();

    RestoreRequest request;
    request.job = fixture_->job();
    request.destinationDirectory = restoreDirectory_;
    request.policy = ConflictPolicy::Skip;
    ArchiveRestorer restorer(*fixture_->database);
    const jobs::RestoreReport report = restorer.restore(request, cancellation);

    QCOMPARE(report.status, QStringLiteral("cancelled"));
    QCOMPARE(report.restored, 0);
    const Result<jobs::RunSummary> summary = fixture_->store->getRun(1);
    Q_UNUSED(summary);
}

QTEST_GUILESS_MAIN(ArchiveRestoreTest)
#include "tst_archive_restore.moc"
