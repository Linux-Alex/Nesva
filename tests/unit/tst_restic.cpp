// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>

#include <functional>
#include <memory>

#include "nesva/backup/restic.hpp"
#include "nesva/core/external_tool.hpp"
#include "nesva/core/paths.hpp"
#include "support/test_fixtures.hpp"

using namespace nesva;
using namespace nesva::backup;

namespace {

/// Scripted runner: records argv and returns canned output, so argument
/// construction, parsing and error mapping are tested without a repository.
class FakeRunner final : public ResticRunner {
public:
    struct Call {
        QStringList arguments;
    };

    QVector<Call> calls;
    std::function<ResticOutcome(const QStringList&, int)> responder;

    ResticOutcome run(const QStringList& arguments, const CancellationToken& cancellation,
                      int timeoutMs, const std::function<void(const QByteArray&)>& onLine) override {
        Q_UNUSED(cancellation);
        Q_UNUSED(timeoutMs);
        calls.append({arguments});
        ResticOutcome outcome = responder ? responder(arguments, static_cast<int>(calls.size()) - 1)
                                          : ok();
        if (onLine) {
            for (const QByteArray& line : outcome.standardOutput.split('\n')) {
                if (!line.trimmed().isEmpty()) {
                    onLine(line.trimmed());
                }
            }
        }
        return outcome;
    }

    static ResticOutcome ok(const QByteArray& output = {}) {
        ResticOutcome outcome;
        outcome.exitCode = 0;
        outcome.standardOutput = output;
        return outcome;
    }

    static ResticOutcome fail(int code, const QByteArray& stderrText) {
        ResticOutcome outcome;
        outcome.exitCode = code;
        outcome.standardError = stderrText;
        return outcome;
    }
};

QByteArray snapshotJson() {
    return R"([{"time":"2026-01-02T03:04:05.123456789+01:00","tree":"abc","paths":["/data/docs"],
      "hostname":"probe","username":"u","tags":["nesva"],"program_version":"restic 0.18.1",
      "summary":{"files_new":3,"total_files_processed":3,"total_bytes_processed":12345},
      "id":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "short_id":"01234567"}])";
}

} // namespace

class ResticTest : public QObject {
    Q_OBJECT

private slots:
    void init();

    // ---- argument construction (fake runner) -------------------------------
    void buildsBaseArguments();
    void buildsInitArguments();
    void buildsBackupArgumentsWithExclusionsAndLimit();
    void buildsRestoreArgumentsWithIncludes();
    void buildsForgetArguments();
    void buildsCheckArguments();
    void initReportsLockedRepository();

    // ---- parsing ----------------------------------------------------------
    void parsesSnapshots();
    void parsesBackupStatusAndSummary();
    void reportsBackupErrors();
    void parsesListNodes();
    void parsesForgetOutputWithNullRemove();
    void parsesCheckSummary();
    void refusesEmptyRetentionPolicy();
    void retentionJsonRoundTrip();

    // ---- error mapping ----------------------------------------------------
    void mapsDocumentedExitCodes();

    // ---- password file ----------------------------------------------------
    void createsRestrictedPasswordFile();

    // ---- real restic integration -----------------------------------------
    void realRepositoryLifecycle();
    void realBackupIsIncremental();
    void realRetentionDryRunAndExecution();
    void realCheckAndRestore();
    void realMissingRepositoryMapsToNotFound();

private:
    static bool resticAvailable() {
        return !resolveExecutable(QStringLiteral("restic")).isEmpty();
    }

private:
    std::unique_ptr<QTemporaryDir> directory_;
};

void ResticTest::init() {
    directory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(directory_->isValid());
    Paths::setDataRootOverride(directory_->path());
    QVERIFY(Paths::ensureBaseDirectories().ok());
}

void ResticTest::buildsBaseArguments() {
    const QStringList arguments = command::init(QStringLiteral("/srv/repo"), QStringLiteral("/tmp/pw"));
    QCOMPARE(arguments.at(0), QStringLiteral("--repo"));
    QCOMPARE(arguments.at(1), QStringLiteral("/srv/repo"));
    QCOMPARE(arguments.at(2), QStringLiteral("--password-file"));
    QCOMPARE(arguments.at(3), QStringLiteral("/tmp/pw"));
    QCOMPARE(arguments.at(4), QStringLiteral("init"));
}

void ResticTest::buildsInitArguments() {
    const QStringList arguments = command::init(QStringLiteral("rclone:icloud:Nesva/restic"), QString());
    QCOMPARE(arguments.at(1), QStringLiteral("rclone:icloud:Nesva/restic"));
    // No password file means restic falls back to its own configuration.
    QVERIFY(!arguments.contains(QStringLiteral("--password-file")));
    QCOMPARE(arguments.last(), QStringLiteral("init"));
}

void ResticTest::buildsBackupArgumentsWithExclusionsAndLimit() {
    const QVector<scan::ExclusionRule> exclusions = {
        {QStringLiteral("*.tmp"), scan::PatternKind::Glob},
        {QStringLiteral(".*\\\\.log$"), scan::PatternKind::Regex},
    };
    const QStringList arguments = command::backup(
        QStringLiteral("/srv/repo"), QStringLiteral("/tmp/pw"),
        {QStringLiteral("/data/docs"), QStringLiteral("/data/Photos with space")}, exclusions,
        {QStringLiteral("nesva")}, 512);

    QVERIFY(arguments.contains(QStringLiteral("backup")));
    QVERIFY(arguments.contains(QStringLiteral("--json")));
    QVERIFY(arguments.contains(QStringLiteral("--exclude")));
    QVERIFY(arguments.contains(QStringLiteral("*.tmp")));
    QVERIFY(arguments.contains(QStringLiteral("--iexclude")));
    QVERIFY(arguments.contains(QStringLiteral("--limit-upload")));
    QVERIFY(arguments.contains(QStringLiteral("512")));
    QVERIFY(arguments.contains(QStringLiteral("--tag")));
    QVERIFY(arguments.contains(QStringLiteral("nesva")));
    // Paths are single arguments, spaces included, and never shell-quoted.
    QVERIFY(arguments.contains(QStringLiteral("/data/Photos with space")));
    for (const QString& argument : arguments) {
        QVERIFY(!argument.contains(QLatin1Char('"')));
        QVERIFY(!argument.contains(QStringLiteral("&&")));
    }
}

void ResticTest::buildsRestoreArgumentsWithIncludes() {
    const QStringList arguments = command::restore(
        QStringLiteral("/srv/repo"), QStringLiteral("/tmp/pw"), QStringLiteral("abc123"),
        QStringLiteral("/tmp/target"), {QStringLiteral("/data/docs/a.txt")});
    QVERIFY(arguments.contains(QStringLiteral("restore")));
    QVERIFY(arguments.contains(QStringLiteral("--target")));
    QVERIFY(arguments.contains(QStringLiteral("/tmp/target")));
    QVERIFY(arguments.contains(QStringLiteral("--include")));
    QVERIFY(arguments.contains(QStringLiteral("/data/docs/a.txt")));
}

void ResticTest::buildsForgetArguments() {
    RetentionPolicy policy;
    policy.keepDaily = 7;
    policy.keepWeekly = 4;
    policy.keepMonthly = 6;
    policy.keepYearly = 0;
    policy.keepLast = 2;

    const QStringList dry = command::forget(QStringLiteral("/srv/repo"), QStringLiteral("/tmp/pw"),
                                            policy, true, false);
    QVERIFY(dry.contains(QStringLiteral("forget")));
    QVERIFY(dry.contains(QStringLiteral("--json")));
    QVERIFY(dry.contains(QStringLiteral("--dry-run")));
    QVERIFY(!dry.contains(QStringLiteral("--prune")));
    QVERIFY(dry.contains(QStringLiteral("--keep-daily")));
    QVERIFY(dry.contains(QStringLiteral("7")));

    const QStringList apply = command::forget(QStringLiteral("/srv/repo"), QStringLiteral("/tmp/pw"),
                                              policy, false, true);
    QVERIFY(!apply.contains(QStringLiteral("--dry-run")));
    QVERIFY(apply.contains(QStringLiteral("--prune")));
}

void ResticTest::buildsCheckArguments() {
    const QStringList arguments =
        command::check(QStringLiteral("/srv/repo"), QStringLiteral("/tmp/pw"), QStringLiteral("5%"));
    QVERIFY(arguments.contains(QStringLiteral("check")));
    QVERIFY(arguments.contains(QStringLiteral("--read-data-subset")));
    QVERIFY(arguments.contains(QStringLiteral("5%")));
}

void ResticTest::initReportsLockedRepository() {
    auto runner = std::make_shared<FakeRunner>();
    runner->responder = [](const QStringList&, int) {
        return FakeRunner::fail(11, "repository is already locked exclusively by PID 42 on host");
    };
    ResticRepository repository(QStringLiteral("restic"), QStringLiteral("/srv/repo"),
                                QStringLiteral("/tmp/pw"), runner);
    const Status status = repository.init();
    QVERIFY(!status.ok());
    QCOMPARE(static_cast<int>(status.error().code()), static_cast<int>(ErrorCode::AlreadyExists));
    QVERIFY(status.error().message().find("locked") != std::string::npos);
}

void ResticTest::parsesSnapshots() {
    const Result<QVector<SnapshotInfo>> snapshots = ResticRepository::parseSnapshots(snapshotJson());
    QVERIFY(snapshots);
    QCOMPARE(snapshots.value().size(), 1);
    const SnapshotInfo& info = snapshots.value().constFirst();
    QCOMPARE(info.shortId, QStringLiteral("01234567"));
    QCOMPARE(info.id.size(), 64);
    QCOMPARE(info.hostname, QStringLiteral("probe"));
    QCOMPARE(info.paths, QStringList{QStringLiteral("/data/docs")});
    QCOMPARE(info.tags, QStringList{QStringLiteral("nesva")});
    QCOMPARE(info.totalBytes, qint64(12345));
    QVERIFY(info.time.isValid());
    QCOMPARE(static_cast<int>(info.time.timeSpec()), static_cast<int>(Qt::UTC));
    QCOMPARE(info.time.offsetFromUtc(), 0);
}

void ResticTest::parsesBackupStatusAndSummary() {
    const QVector<QByteArray> lines = {
        R"({"message_type":"status","percent_done":0.25,"files_done":1,"total_files":4,"bytes_done":10,"total_bytes":40,"current_files":["/data/a"]})",
        R"({"message_type":"status","percent_done":1.0,"files_done":4,"total_files":4,"bytes_done":40,"total_bytes":40})",
        R"({"message_type":"summary","files_new":3,"files_changed":1,"files_unmodified":0,"total_files_processed":4,"total_bytes_processed":40,"data_added":99,"backup_start":"2026-01-02T03:04:05Z","backup_end":"2026-01-02T03:04:09Z","snapshot_id":"deadbeef"})",
    };

    BackupProgress progress;
    const Result<BackupSummary> summary = ResticRepository::parseBackupLines(lines, &progress);
    QVERIFY(summary);
    QCOMPARE(summary.value().snapshotId, QStringLiteral("deadbeef"));
    QCOMPARE(summary.value().filesNew, qint64(3));
    QCOMPARE(summary.value().filesChanged, qint64(1));
    QCOMPARE(summary.value().totalFilesProcessed, qint64(4));
    QCOMPARE(summary.value().dataAdded, qint64(99));
    QVERIFY(summary.value().finishedUtc.isValid());
    QCOMPARE(progress.filesDone, qint64(4));
}

void ResticTest::reportsBackupErrors() {
    const QVector<QByteArray> lines = {
        R"({"message_type":"error","message":"cannot read file: permission denied"})",
    };
    const Result<BackupSummary> summary = ResticRepository::parseBackupLines(lines);
    QVERIFY(!summary);
    QCOMPARE(static_cast<int>(summary.error().code()), static_cast<int>(ErrorCode::ExternalToolFailed));
    QVERIFY(summary.error().detail().find("permission denied") != std::string::npos);

    // A run without a summary must not be reported as success.
    const Result<BackupSummary> truncated = ResticRepository::parseBackupLines({});
    QVERIFY(!truncated);
}

void ResticTest::parsesListNodes() {
    const QVector<QByteArray> lines = {
        R"({"time":"2026-01-02T03:04:05Z","tree":"abc","paths":["/data/docs"]})",
        R"({"name":"docs","type":"dir","path":"/data/docs","mtime":"2026-01-02T03:04:05Z","message_type":"node"})",
        R"({"name":"a.txt","type":"file","path":"/data/docs/a.txt","size":6,"mtime":"2026-01-02T03:04:05Z","message_type":"node"})",
    };
    const Result<QVector<FileEntry>> entries = ResticRepository::parseListLines(lines);
    QVERIFY(entries);
    QCOMPARE(entries.value().size(), 2);
    QCOMPARE(entries.value().at(0).type, QStringLiteral("dir"));
    QCOMPARE(entries.value().at(1).path, QStringLiteral("/data/docs/a.txt"));
    QCOMPARE(entries.value().at(1).sizeBytes, qint64(6));
}

void ResticTest::parsesForgetOutputWithNullRemove() {
    const QByteArray json = R"([{"tags":null,"host":"probe","paths":["/data"],"reasons":{},
        "keep":[{"id":"aaa","short_id":"aaa"},{"id":"bbb"}],"remove":null},
        {"tags":null,"host":"other","paths":["/data"],"keep":[],"remove":[{"id":"ccc"}]}])";
    const Result<QVector<ForgetGroup>> groups = ResticRepository::parseForgetOutput(json);
    QVERIFY(groups);
    QCOMPARE(groups.value().size(), 2);
    QCOMPARE(groups.value().at(0).keptIds.size(), 2);
    QCOMPARE(groups.value().at(0).removedIds.size(), 0);
    QCOMPARE(groups.value().at(1).removedIds, QStringList{QStringLiteral("ccc")});
}

void ResticTest::parsesCheckSummary() {
    const QVector<QByteArray> lines = {
        R"({"message_type":"status","percent_done":0.5})",
        R"({"message_type":"error","message":"pack 123: invalid checksum"})",
        R"({"message_type":"summary","num_errors":1,"broken_packs":[],"suggest_prune":false})",
    };
    const Result<VerifyResult> result = ResticRepository::parseCheckLines(lines);
    QVERIFY(result);
    QVERIFY(!result.value().ok);
    QCOMPARE(result.value().errors, qint64(1));
    QCOMPARE(result.value().messages.size(), 1);
}

void ResticTest::refusesEmptyRetentionPolicy() {
    auto runner = std::make_shared<FakeRunner>();
    ResticRepository repository(QStringLiteral("restic"), QStringLiteral("/srv/repo"),
                                QStringLiteral("/tmp/pw"), runner);
    RetentionPolicy policy;
    policy.keepLast = 0;
    policy.keepDaily = 0;
    policy.keepWeekly = 0;
    policy.keepMonthly = 0;
    policy.keepYearly = 0;
    const Result<QVector<ForgetGroup>> groups = repository.forget(policy, true, false);
    QVERIFY(!groups);
    QCOMPARE(static_cast<int>(groups.error().code()), static_cast<int>(ErrorCode::InvalidArgument));
    QCOMPARE(runner->calls.size(), 0);
}

void ResticTest::retentionJsonRoundTrip() {
    RetentionPolicy policy;
    policy.keepDaily = 7;
    policy.keepWeekly = 4;
    policy.keepMonthly = 6;
    policy.keepYearly = 2;
    policy.keepLast = 1;
    const RetentionPolicy parsed = retentionFromJson(retentionToJson(policy));
    QCOMPARE(parsed.keepDaily, 7);
    QCOMPARE(parsed.keepWeekly, 4);
    QCOMPARE(parsed.keepMonthly, 6);
    QCOMPARE(parsed.keepYearly, 2);
    QCOMPARE(parsed.keepLast, 1);
}

void ResticTest::mapsDocumentedExitCodes() {
    ResticOutcome outcome;
    outcome.exitCode = 10;
    QCOMPARE(static_cast<int>(mapResticFailure(outcome, QStringLiteral("op")).code()),
             static_cast<int>(ErrorCode::NotFound));
    outcome.exitCode = 11;
    QCOMPARE(static_cast<int>(mapResticFailure(outcome, QStringLiteral("op")).code()),
             static_cast<int>(ErrorCode::AlreadyExists));
    outcome.exitCode = 12;
    QCOMPARE(static_cast<int>(mapResticFailure(outcome, QStringLiteral("op")).code()),
             static_cast<int>(ErrorCode::PermissionDenied));
    outcome.exitCode = 1;
    QCOMPARE(static_cast<int>(mapResticFailure(outcome, QStringLiteral("op")).code()),
             static_cast<int>(ErrorCode::ExternalToolFailed));

    outcome.exitCode = -1;
    outcome.cancelled = true;
    QCOMPARE(static_cast<int>(mapResticFailure(outcome, QStringLiteral("op")).code()),
             static_cast<int>(ErrorCode::Cancelled));

    ResticOutcome notStarted;
    notStarted.started = false;
    notStarted.startError = QStringLiteral("missing binary");
    QCOMPARE(static_cast<int>(mapResticFailure(notStarted, QStringLiteral("op")).code()),
             static_cast<int>(ErrorCode::ExternalToolFailed));
}

void ResticTest::createsRestrictedPasswordFile() {
    const Result<QString> path = createPasswordFile(QByteArray("very-secret"));
    QVERIFY(path);
    const QFileInfo info(path.value());
    QVERIFY(info.exists());
    QVERIFY(!(info.permissions() & QFileDevice::ReadGroup));
    QVERIFY(!(info.permissions() & QFileDevice::ReadOther));
    QCOMPARE(test::readFile(path.value()), QByteArray("very-secret"));
    QFile::remove(path.value());
}

// ---------------------------------------------------------------------------
// Real restic integration (skipped when restic is not installed)
// ---------------------------------------------------------------------------

void ResticTest::realRepositoryLifecycle() {
    if (!resticAvailable()) {
        QSKIP("restic is not installed");
    }
    const QString passwordFile = createPasswordFile(QByteArray("unit-test-password")).value();
    const QString repo = directory_->filePath(QStringLiteral("repo"));
    const QString source = directory_->filePath(QStringLiteral("src"));
    QVERIFY(QDir().mkpath(source));
    QVERIFY(test::writeFile(QDir(source).absoluteFilePath(QStringLiteral("a.txt")), "alpha"));

    ResticRepository repository(QStringLiteral("restic"), repo, passwordFile);
    QVERIFY2(repository.init().ok(), "restic init should create the repository");

    const CancellationToken cancellation;
    const Result<BackupSummary> summary = repository.backup(
        {source}, {}, {QStringLiteral("nesva")}, 0, cancellation);
    QVERIFY2(summary.hasValue(),
             summary.hasValue() ? "" : summary.error().toDisplayString().c_str());
    QVERIFY(!summary.value().snapshotId.isEmpty());
    QCOMPARE(summary.value().filesNew, qint64(1));

    const Result<QVector<SnapshotInfo>> snapshots = repository.snapshots({QStringLiteral("nesva")});
    QVERIFY(snapshots);
    QCOMPARE(snapshots.value().size(), 1);
    QCOMPARE(snapshots.value().constFirst().id, summary.value().snapshotId);

    const Result<QVector<FileEntry>> entries = repository.listSnapshot(summary.value().snapshotId);
    QVERIFY(entries);
    QVERIFY(!entries.value().isEmpty());
    bool foundFile = false;
    for (const FileEntry& entry : entries.value()) {
        if (entry.path.endsWith(QStringLiteral("a.txt")) && entry.type == QStringLiteral("file")) {
            foundFile = true;
        }
    }
    QVERIFY(foundFile);

    const Result<QVector<LockInfo>> locks = repository.listLocks();
    QVERIFY(locks);
    QCOMPARE(locks.value().size(), 0); // no stale locks after a clean run

    QFile::remove(passwordFile);
}

void ResticTest::realBackupIsIncremental() {
    if (!resticAvailable()) {
        QSKIP("restic is not installed");
    }
    const QString passwordFile = createPasswordFile(QByteArray("unit-test-password")).value();
    const QString repo = directory_->filePath(QStringLiteral("repo2"));
    const QString source = directory_->filePath(QStringLiteral("src2"));
    QVERIFY(QDir().mkpath(source));
    QVERIFY(test::writeFile(QDir(source).absoluteFilePath(QStringLiteral("a.txt")), "alpha"));
    QVERIFY(test::writeFile(QDir(source).absoluteFilePath(QStringLiteral("b.txt")), "bravo"));

    ResticRepository repository(QStringLiteral("restic"), repo, passwordFile);
    QVERIFY(repository.init().ok());
    const CancellationToken cancellation;

    const Result<BackupSummary> first =
        repository.backup({source}, {}, {QStringLiteral("nesva")}, 0, cancellation);
    QVERIFY(first);
    QCOMPARE(first.value().filesNew, qint64(2));

    const Result<BackupSummary> second =
        repository.backup({source}, {}, {QStringLiteral("nesva")}, 0, cancellation);
    QVERIFY(second);
    QCOMPARE(second.value().filesNew, qint64(0));
    QCOMPARE(second.value().filesUnmodified, qint64(2));

    QVERIFY(test::writeFile(QDir(source).absoluteFilePath(QStringLiteral("a.txt")), "alpha v2"));
    const Result<BackupSummary> third =
        repository.backup({source}, {}, {QStringLiteral("nesva")}, 0, cancellation);
    QVERIFY(third);
    QCOMPARE(third.value().filesChanged, qint64(1));

    QFile::remove(passwordFile);
}

void ResticTest::realRetentionDryRunAndExecution() {
    if (!resticAvailable()) {
        QSKIP("restic is not installed");
    }
    const QString passwordFile = createPasswordFile(QByteArray("unit-test-password")).value();
    const QString repo = directory_->filePath(QStringLiteral("repo3"));
    const QString source = directory_->filePath(QStringLiteral("src3"));
    QVERIFY(QDir().mkpath(source));
    QVERIFY(test::writeFile(QDir(source).absoluteFilePath(QStringLiteral("a.txt")), "alpha"));

    ResticRepository repository(QStringLiteral("restic"), repo, passwordFile);
    QVERIFY(repository.init().ok());
    const CancellationToken cancellation;
    for (int index = 0; index < 3; ++index) {
        QVERIFY(test::writeFile(QDir(source).absoluteFilePath(QStringLiteral("a.txt")),
                                QByteArray("alpha ") + QByteArray::number(index)));
        QVERIFY(repository.backup({source}, {}, {QStringLiteral("nesva")}, 0, cancellation)
                    .hasValue());
    }

    RetentionPolicy policy;
    policy.keepLast = 1;
    policy.keepDaily = 0;
    policy.keepWeekly = 0;
    policy.keepMonthly = 0;
    policy.keepYearly = 0;

    const Result<QVector<ForgetGroup>> dry = repository.forget(policy, true, false);
    QVERIFY2(dry.hasValue(), dry.hasValue() ? "" : dry.error().toDisplayString().c_str());
    qint64 wouldRemove = 0;
    for (const ForgetGroup& group : dry.value()) {
        wouldRemove += group.removedIds.size();
    }
    QCOMPARE(wouldRemove, qint64(2));

    const Result<QVector<SnapshotInfo>> before = repository.snapshots();
    QVERIFY(before);
    QCOMPARE(before.value().size(), 3);

    const Result<QVector<ForgetGroup>> applied = repository.forget(policy, false, false);
    QVERIFY(applied);
    const Result<QVector<SnapshotInfo>> after = repository.snapshots();
    QVERIFY(after);
    QCOMPARE(after.value().size(), 1);

    QFile::remove(passwordFile);
}

void ResticTest::realCheckAndRestore() {
    if (!resticAvailable()) {
        QSKIP("restic is not installed");
    }
    const QString passwordFile = createPasswordFile(QByteArray("unit-test-password")).value();
    const QString repo = directory_->filePath(QStringLiteral("repo4"));
    const QString source = directory_->filePath(QStringLiteral("src4"));
    const QString target = directory_->filePath(QStringLiteral("restore4"));
    QVERIFY(QDir().mkpath(source));
    QVERIFY(test::writeFile(QDir(source).absoluteFilePath(QStringLiteral("a.txt")), "alpha"));

    ResticRepository repository(QStringLiteral("restic"), repo, passwordFile);
    QVERIFY(repository.init().ok());
    const CancellationToken cancellation;
    const Result<BackupSummary> summary =
        repository.backup({source}, {}, {QStringLiteral("nesva")}, 0, cancellation);
    QVERIFY(summary);

    const Result<VerifyResult> check = repository.check(QStringLiteral("100%"));
    QVERIFY2(check.hasValue(), check.hasValue() ? "" : check.error().toDisplayString().c_str());
    QVERIFY(check.value().ok);
    QCOMPARE(check.value().errors, qint64(0));

    const Result<QStringList> restored =
        repository.restore(summary.value().snapshotId, target, {}, cancellation);
    QVERIFY2(restored.hasValue(), restored.hasValue() ? "" : restored.error().toDisplayString().c_str());
    // restic restores the original absolute layout under the target.
    const QString restoredFile = QDir(target).absoluteFilePath(source.mid(1) + QStringLiteral("/a.txt"));
    QVERIFY2(QFileInfo::exists(restoredFile), qPrintable(restoredFile));
    QCOMPARE(test::readFile(restoredFile), QByteArray("alpha"));

    QFile::remove(passwordFile);
}

void ResticTest::realMissingRepositoryMapsToNotFound() {
    if (!resticAvailable()) {
        QSKIP("restic is not installed");
    }
    const QString passwordFile = createPasswordFile(QByteArray("unit-test-password")).value();
    ResticRepository repository(QStringLiteral("restic"),
                                directory_->filePath(QStringLiteral("no-such-repo")), passwordFile);
    const Result<QVector<SnapshotInfo>> snapshots = repository.snapshots();
    QVERIFY(!snapshots);
    QCOMPARE(static_cast<int>(snapshots.error().code()), static_cast<int>(ErrorCode::NotFound));
    QFile::remove(passwordFile);
}

QTEST_GUILESS_MAIN(ResticTest)
#include "tst_restic.moc"
