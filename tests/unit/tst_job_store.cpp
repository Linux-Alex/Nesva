// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include <utility>

#include "nesva/core/process_utils.hpp"
#include "nesva/db/database.hpp"
#include "nesva/jobs/job_store.hpp"

using namespace nesva;
using nesva::db::Database;
using nesva::jobs::ArchiveJob;
using nesva::jobs::JobSource;
using nesva::jobs::JobStore;
using nesva::jobs::RunSummary;
using nesva::jobs::StorageProviderRecord;

class JobStoreTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void createsAndReadsJobs();
    void derivesUniqueSourceLabels();
    void updatesJobAndReplacesSources();
    void deletesJobAndCascades();
    void rejectsInvalidJobs();
    void rejectsBackupModeForNow();
    void storesLocalProvider();
    void preventsDuplicateRunningRuns();
    void finishesRunWithCounters();
    void recoversRunsFromDeadProcesses();
    void keepsRunsOwnedByOtherHosts();
    void recordsManifestAndReadsItBack();
    void updatesManifestEntryInPlace();
    void recordsArchiveObjects();

private:
    QTemporaryDir* directory_ = nullptr;
    Database* database_ = nullptr;
    JobStore* store_ = nullptr;

    qint64 createJob(const QString& name, const QString& sourcePath) {
        ArchiveJob job;
        job.name = name;
        job.mode = QStringLiteral("archive");
        job.destinationPath = directory_->filePath(QStringLiteral("dest"));
        job.sources = {{0, sourcePath, QString()}};
        Result<qint64> created = store_->createJob(job);
        return created ? created.value() : 0;
    }
};

void JobStoreTest::init() {
    directory_ = new QTemporaryDir();
    QVERIFY(directory_->isValid());
    database_ = new Database();
    QVERIFY(database_->open(directory_->filePath(QStringLiteral("nesva.db"))).ok());
    QVERIFY(database_->migrate().ok());
    store_ = new JobStore(*database_);
}

void JobStoreTest::cleanup() {
    delete store_;
    delete database_;
    delete directory_;
    store_ = nullptr;
    database_ = nullptr;
    directory_ = nullptr;
}

void JobStoreTest::createsAndReadsJobs() {
    const qint64 first = createJob(QStringLiteral("Documents"), QStringLiteral("/tmp/docs"));
    const qint64 second = createJob(QStringLiteral("Photos"), QStringLiteral("/tmp/photos"));
    QVERIFY(first > 0);
    QVERIFY(second > 0);
    QVERIFY(first != second);

    const Result<QVector<ArchiveJob>> jobs = store_->listJobs();
    QVERIFY(jobs);
    QCOMPARE(jobs.value().size(), 2);
    // Ordered by name.
    QCOMPARE(jobs.value().at(0).name, QStringLiteral("Documents"));

    const Result<ArchiveJob> loaded = store_->getJob(first);
    QVERIFY(loaded);
    QCOMPARE(loaded.value().name, QStringLiteral("Documents"));
    QCOMPARE(loaded.value().mode, QStringLiteral("archive"));
    QCOMPARE(loaded.value().sources.size(), 1);
    QCOMPARE(loaded.value().sources.constFirst().path, QStringLiteral("/tmp/docs"));

    const Result<ArchiveJob> missing = store_->getJob(4242);
    QVERIFY(!missing);
    QCOMPARE(static_cast<int>(missing.error().code()), static_cast<int>(ErrorCode::NotFound));
}

void JobStoreTest::derivesUniqueSourceLabels() {
    ArchiveJob job;
    job.name = QStringLiteral("Two sources");
    job.destinationPath = directory_->filePath(QStringLiteral("dest"));
    job.sources = {{0, QStringLiteral("/data/photos"), QString()},
                   {0, QStringLiteral("/backup/photos"), QString()},
                   {0, QStringLiteral("/data/Music Library"), QString()}};
    const Result<qint64> created = store_->createJob(job);
    QVERIFY(created);

    const Result<ArchiveJob> loaded = store_->getJob(created.value());
    QVERIFY(loaded);
    QCOMPARE(loaded.value().sources.size(), 3);
    QCOMPARE(loaded.value().sources.at(0).label, QStringLiteral("photos"));
    QCOMPARE(loaded.value().sources.at(1).label, QStringLiteral("photos-2"));
    // Spaces are normalised so remote paths stay predictable.
    QCOMPARE(loaded.value().sources.at(2).label, QStringLiteral("Music_Library"));
}

void JobStoreTest::updatesJobAndReplacesSources() {
    const qint64 id = createJob(QStringLiteral("Job"), QStringLiteral("/tmp/a"));
    QVERIFY(id > 0);

    Result<ArchiveJob> job = store_->getJob(id);
    QVERIFY(job);
    ArchiveJob updated = job.value();
    updated.name = QStringLiteral("Job renamed");
    updated.sources = {{0, QStringLiteral("/tmp/b"), QString()}};
    updated.exclusions = {{QStringLiteral("*.tmp"), scan::PatternKind::Glob}};
    updated.bandwidthLimitKiB = 512;
    QVERIFY(store_->updateJob(updated).ok());

    Result<ArchiveJob> reloaded = store_->getJob(id);
    QVERIFY(reloaded);
    QCOMPARE(reloaded.value().name, QStringLiteral("Job renamed"));
    QCOMPARE(reloaded.value().sources.size(), 1);
    QCOMPARE(reloaded.value().sources.constFirst().path, QStringLiteral("/tmp/b"));
    QCOMPARE(reloaded.value().exclusions.size(), 1);
    QCOMPARE(reloaded.value().bandwidthLimitKiB, 512);
}

void JobStoreTest::deletesJobAndCascades() {
    const qint64 id = createJob(QStringLiteral("Disposable"), QStringLiteral("/tmp/x"));
    QVERIFY(id > 0);
    QVERIFY(store_->deleteJob(id).ok());

    Result<ArchiveJob> missing = store_->getJob(id);
    QVERIFY(!missing);

    Result<QSqlQuery> sources = database_->select(
        QStringLiteral("SELECT COUNT(*) FROM backup_sources WHERE job_id = ?"), {QVariant(id)});
    QVERIFY(sources);
    QSqlQuery rows = std::move(sources).take();
    QVERIFY(rows.next());
    QCOMPARE(rows.value(0).toInt(), 0);
}

void JobStoreTest::rejectsInvalidJobs() {
    ArchiveJob nameless;
    nameless.destinationPath = directory_->filePath(QStringLiteral("dest"));
    nameless.sources = {{0, QStringLiteral("/tmp/x"), QString()}};
    QVERIFY(!store_->createJob(nameless).hasValue());

    ArchiveJob sourceless;
    sourceless.name = QStringLiteral("No sources");
    sourceless.destinationPath = directory_->filePath(QStringLiteral("dest"));
    const Result<qint64> created = store_->createJob(sourceless);
    QVERIFY(!created);
    QCOMPARE(static_cast<int>(created.error().code()), static_cast<int>(ErrorCode::InvalidArgument));
}

void JobStoreTest::rejectsBackupModeForNow() {
    ArchiveJob job;
    job.name = QStringLiteral("Backup mode");
    job.mode = QStringLiteral("backup");
    job.destinationPath = directory_->filePath(QStringLiteral("repo"));
    job.sources = {{0, QStringLiteral("/tmp/x"), QString()}};
    const Result<qint64> created = store_->createJob(job);
    QVERIFY(!created);
    QCOMPARE(static_cast<int>(created.error().code()), static_cast<int>(ErrorCode::NotImplemented));
}

void JobStoreTest::storesLocalProvider() {
    const Result<qint64> id =
        store_->upsertLocalProvider(QStringLiteral("External disk"), QStringLiteral("/mnt/archive"));
    QVERIFY(id);
    QVERIFY(id.value() > 0);

    const Result<StorageProviderRecord> provider = store_->getProvider(id.value());
    QVERIFY(provider);
    QCOMPARE(provider.value().providerType, QStringLiteral("local"));
    QCOMPARE(provider.value().basePath, QStringLiteral("/mnt/archive"));

    // Upsert by name updates in place instead of duplicating.
    const Result<qint64> updated =
        store_->upsertLocalProvider(QStringLiteral("External disk"), QStringLiteral("/mnt/archive2"));
    QVERIFY(updated);
    QCOMPARE(updated.value(), id.value());

    const Result<QVector<StorageProviderRecord>> all = store_->listProviders();
    QVERIFY(all);
    QCOMPARE(all.value().size(), 1);
    QCOMPARE(all.value().constFirst().basePath, QStringLiteral("/mnt/archive2"));
}

void JobStoreTest::preventsDuplicateRunningRuns() {
    const qint64 id = createJob(QStringLiteral("Single flight"), QStringLiteral("/tmp/x"));
    QVERIFY(id > 0);

    const Result<qint64> first = store_->beginRun(id, QStringLiteral("manual"));
    QVERIFY(first);
    QVERIFY(first.value() > 0);

    const Result<qint64> second = store_->beginRun(id, QStringLiteral("manual"));
    QVERIFY(!second);
    QCOMPARE(static_cast<int>(second.error().code()), static_cast<int>(ErrorCode::AlreadyExists));

    const Result<bool> running = store_->isJobRunning(id);
    QVERIFY(running);
    QVERIFY(running.value());

    QVERIFY(store_->finishRun(first.value(), QStringLiteral("succeeded"), {}).ok());
    const Result<bool> stillRunning = store_->isJobRunning(id);
    QVERIFY(stillRunning);
    QVERIFY(!stillRunning.value());

    // Once finished, a new run is allowed and the run number increments.
    const Result<qint64> third = store_->beginRun(id, QStringLiteral("manual"));
    QVERIFY(third);
    const Result<QVector<RunSummary>> runs = store_->listRuns(10, id);
    QVERIFY(runs);
    QCOMPARE(runs.value().size(), 2);
    QCOMPARE(runs.value().at(0).runNumber, qint64(2));
}

void JobStoreTest::finishesRunWithCounters() {
    const qint64 id = createJob(QStringLiteral("Counters"), QStringLiteral("/tmp/x"));
    const Result<qint64> run = store_->beginRun(id, QStringLiteral("manual"));
    QVERIFY(run);

    jobs::RunCounters counters;
    counters.filesTotal = 10;
    counters.filesTransferred = 7;
    counters.filesSkipped = 2;
    counters.filesFailed = 1;
    counters.bytesTotal = 1000;
    counters.bytesTransferred = 700;
    QVERIFY(store_->finishRun(run.value(), QStringLiteral("failed"), counters,
                              Error(ErrorCode::IoError, "one file failed")).ok());

    const Result<RunSummary> summary = store_->getRun(run.value());
    QVERIFY(summary);
    QCOMPARE(summary.value().status, QStringLiteral("failed"));
    QCOMPARE(summary.value().counters.filesTransferred, qint64(7));
    QCOMPARE(summary.value().counters.filesSkipped, qint64(2));
    QCOMPARE(summary.value().errorCode, QStringLiteral("io_error"));
    QVERIFY(!summary.value().finishedAt.isEmpty());
}

void JobStoreTest::recoversRunsFromDeadProcesses() {
    const qint64 id = createJob(QStringLiteral("Stale"), QStringLiteral("/tmp/x"));
    const Result<qint64> run = store_->beginRun(id, QStringLiteral("manual"));
    QVERIFY(run);

    // Simulate a crashed process by pointing the run at a pid that cannot exist.
    QSqlQuery update(database_->handle());
    update.prepare(QStringLiteral("UPDATE job_runs SET owner_pid = 999999 WHERE id = ?"));
    update.addBindValue(run.value());
    QVERIFY(update.exec());

    const Result<int> recovered = store_->recoverStaleRuns();
    QVERIFY(recovered);
    QCOMPARE(recovered.value(), 1);

    const Result<RunSummary> summary = store_->getRun(run.value());
    QVERIFY(summary);
    QCOMPARE(summary.value().status, QStringLiteral("interrupted"));

    // A run owned by this live process is untouched.
    const Result<qint64> live = store_->beginRun(id, QStringLiteral("manual"));
    QVERIFY(live);
    const Result<int> again = store_->recoverStaleRuns();
    QVERIFY(again);
    QCOMPARE(again.value(), 0);
    const Result<RunSummary> liveSummary = store_->getRun(live.value());
    QVERIFY(liveSummary);
    QCOMPARE(liveSummary.value().status, QStringLiteral("running"));
}

void JobStoreTest::keepsRunsOwnedByOtherHosts() {
    const qint64 id = createJob(QStringLiteral("Remote host"), QStringLiteral("/tmp/x"));
    const Result<qint64> run = store_->beginRun(id, QStringLiteral("manual"));
    QVERIFY(run);

    QSqlQuery update(database_->handle());
    update.prepare(QStringLiteral("UPDATE job_runs SET owner_host = 'another-machine' WHERE id = ?"));
    update.addBindValue(run.value());
    QVERIFY(update.exec());

    const Result<int> recovered = store_->recoverStaleRuns();
    QVERIFY(recovered);
    QCOMPARE(recovered.value(), 0);
}

void JobStoreTest::recordsManifestAndReadsItBack() {
    const qint64 id = createJob(QStringLiteral("Manifest"), QStringLiteral("/tmp/x"));
    const Result<qint64> run = store_->beginRun(id, QStringLiteral("manual"));
    QVERIFY(run);

    jobs::ManifestRecord record;
    record.relativePath = QStringLiteral("source/a.txt");
    record.sourcePath = QStringLiteral("/tmp/x/a.txt");
    record.sizeBytes = 5;
    record.modifiedUtc = QStringLiteral("2026-01-01T00:00:00.000Z");
    record.sha256 = QStringLiteral("abc123");
    record.state = QStringLiteral("verified");
    QVERIFY(store_->recordManifestEntry(id, run.value(), record).ok());

    QVERIFY(store_->finishRun(run.value(), QStringLiteral("succeeded"), {}).ok());

    const Result<QMap<QString, jobs::ManifestSnapshot>> manifest =
        store_->lastSuccessfulManifest(id);
    QVERIFY(manifest);
    QCOMPARE(manifest.value().size(), 1);
    QCOMPARE(manifest.value().constBegin()->sizeBytes, qint64(5));

    const Result<QVector<jobs::ManifestRecord>> latest = store_->latestManifest(id);
    QVERIFY(latest);
    QCOMPARE(latest.value().size(), 1);
    QCOMPARE(latest.value().constFirst().sha256, QStringLiteral("abc123"));
    QCOMPARE(latest.value().constFirst().state, QStringLiteral("verified"));
}

void JobStoreTest::updatesManifestEntryInPlace() {
    const qint64 id = createJob(QStringLiteral("Manifest upsert"), QStringLiteral("/tmp/x"));
    const Result<qint64> run = store_->beginRun(id, QStringLiteral("manual"));
    QVERIFY(run);

    jobs::ManifestRecord record;
    record.relativePath = QStringLiteral("source/a.txt");
    record.sourcePath = QStringLiteral("/tmp/x/a.txt");
    record.sizeBytes = 5;
    record.state = QStringLiteral("transferred");
    QVERIFY(store_->recordManifestEntry(id, run.value(), record).ok());

    record.state = QStringLiteral("failed");
    record.skipReason = QStringLiteral("disk full");
    QVERIFY(store_->recordManifestEntry(id, run.value(), record).ok());

    const Result<QVector<jobs::ManifestRecord>> latest = store_->latestManifest(id);
    QVERIFY(latest);
    QCOMPARE(latest.value().size(), 1);
    QCOMPARE(latest.value().constFirst().state, QStringLiteral("failed"));
    QCOMPARE(latest.value().constFirst().skipReason, QStringLiteral("disk full"));
}

void JobStoreTest::recordsArchiveObjects() {
    const qint64 id = createJob(QStringLiteral("Objects"), QStringLiteral("/tmp/x"));
    const Result<qint64> run = store_->beginRun(id, QStringLiteral("manual"));
    QVERIFY(run);

    QVERIFY(store_->recordArchiveObject(id, run.value(), QStringLiteral("source/a.txt"), 5,
                                        QStringLiteral("hash"), QStringLiteral("present"))
                .ok());
    QVERIFY(store_->recordArchiveObject(id, run.value(), QStringLiteral("source/a.txt"), 6,
                                        QStringLiteral("hash2"), QStringLiteral("present"))
                .ok());

    Result<QSqlQuery> query = database_->select(
        QStringLiteral("SELECT COUNT(*), MAX(size_bytes) FROM archive_objects WHERE job_id = ?"),
        {QVariant(id)});
    QVERIFY(query);
    QSqlQuery rows = std::move(query).take();
    QVERIFY(rows.next());
    QCOMPARE(rows.value(0).toInt(), 1);
    QCOMPARE(rows.value(1).toInt(), 6);
}

QTEST_GUILESS_MAIN(JobStoreTest)
#include "tst_job_store.moc"
