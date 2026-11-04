// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include "nesva/core/disk_space.hpp"
#include "nesva/scan/file_scanner.hpp"
#include "nesva/security/identity.hpp"
#include "support/test_fixtures.hpp"

using namespace nesva;

class SafetyTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void serviceAccountCannotRunJobs();
    void userAccountRunsJobs();
    void cloudPlaceholdersAreSkipped();
    void cloudRootsAreNotFollowed();
    void freeSpaceGuardNamesAnotherDisk();

private:
    std::unique_ptr<QTemporaryDir> directory_;
};

void SafetyTest::init() {
    directory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(directory_->isValid());
    security::resetIdentityProviderForTests();
}

void SafetyTest::cleanup() {
    security::resetIdentityProviderForTests();
    directory_.reset();
}

void SafetyTest::serviceAccountCannotRunJobs() {
    // The Windows SYSTEM account has its own profile and no access to the
    // interactive user's keyring: a run started there archives the wrong data.
    security::ProcessIdentity system;
    system.userName = QStringLiteral("NT AUTHORITY\\SYSTEM");
    system.userId = QStringLiteral("S-1-5-18");
    system.serviceAccount = true;
    system.detail = QStringLiteral("running as a Windows service account (S-1-5-18)");
    security::setIdentityProviderForTests([system] { return system; });

    const Status refused = security::requireUserContext(QStringLiteral("job run"));
    QVERIFY(!refused.ok());
    QCOMPARE(static_cast<int>(refused.error().code()),
             static_cast<int>(ErrorCode::PermissionDenied));
    QVERIFY(QString::fromStdString(refused.error().toDisplayString())
                .contains(QStringLiteral("as the user")));
}

void SafetyTest::userAccountRunsJobs() {
    security::ProcessIdentity user;
    user.userName = QStringLiteral("EXAMPLE\\alice");
    user.userId = QStringLiteral("S-1-5-21-2827920605-2240180639-1320108958-1174");
    security::setIdentityProviderForTests([user] { return user; });
    QVERIFY(security::requireUserContext(QStringLiteral("job run")).ok());
}

void SafetyTest::cloudPlaceholdersAreSkipped() {
    // Reading an iCloud/OneDrive placeholder downloads it. The scanner must skip
    // it and say why, instead of "archiving" something that is not local.
    const QString local = directory_->filePath(QStringLiteral("real.txt"));
    const QString placeholder = directory_->filePath(QStringLiteral("cloud.jpg"));
    QVERIFY(test::writeFile(local, "local content"));
    QVERIFY(test::writeFile(placeholder, ""));

    scan::ScanOptions options;
    options.placeholderProbe = [placeholder](const QString& path) { return path == placeholder; };

    const Result<scan::ScanResult> result = scan::scanSources(
        {{directory_->path(), QStringLiteral("root")}}, options, CancellationToken());
    QVERIFY(result.hasValue());
    QCOMPARE(result.value().files.size(), 1);
    QCOMPARE(result.value().files.constFirst().relativePath, QStringLiteral("real.txt"));

    bool reported = false;
    for (const scan::ScanSkip& skip : result.value().skipped) {
        if (skip.path == placeholder) {
            reported = true;
            QVERIFY(skip.reason.contains(QStringLiteral("cloud placeholder")));
        }
    }
    QVERIFY2(reported, "a skipped placeholder must be reported, not silently dropped");
}

void SafetyTest::cloudRootsAreNotFollowed() {
    // iCloud Drive / OneDrive are separate accounts' data; walking one can pull
    // the whole cloud down during a backup.
    const QString root = directory_->filePath(QStringLiteral("iCloudDrive"));
    QVERIFY(QDir().mkpath(root));
    QVERIFY(test::writeFile(root + QStringLiteral("/photo.jpg"), "bytes"));

    scan::ScanOptions options;
    const Result<scan::ScanResult> result = scan::scanSources(
        {{directory_->path(), QStringLiteral("root")}}, options, CancellationToken());
    QVERIFY(result.hasValue());
    QCOMPARE(result.value().files.size(), 0);

    bool reported = false;
    for (const scan::ScanSkip& skip : result.value().skipped) {
        if (skip.path == root) {
            reported = true;
            QVERIFY(skip.reason.contains(QStringLiteral("cloud sync folder")));
        }
    }
    QVERIFY(reported);
}

void SafetyTest::freeSpaceGuardNamesAnotherDisk() {
    // An impossible requirement must fail with an actionable message rather than
    // discovering the problem halfway through a transfer.
    const Status impossible = requireFreeSpace(directory_->path(), 1024LL * 1024 * 1024 * 1024 * 8,
                                               QStringLiteral("archiving this job"));
    QVERIFY(!impossible.ok());
    QCOMPARE(static_cast<int>(impossible.error().code()), static_cast<int>(ErrorCode::IoError));
    const QString detail = QString::fromStdString(impossible.error().detail());
    QVERIFY(!detail.isEmpty() && detail.contains(QStringLiteral("free")));

    // A small requirement passes.
    QVERIFY(requireFreeSpace(directory_->path(), 1024).ok());
}

QTEST_GUILESS_MAIN(SafetyTest)
#include "tst_safety.moc"
