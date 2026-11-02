// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#include "nesva/core/paths.hpp"
#include "support/test_paths.hpp"

using namespace nesva;

class PathsTest : public QObject {
    Q_OBJECT

private slots:
    void cleanup();
    void overrideWinsOverDefaults();
    void environmentVariableUsedWhenNoOverride();
    void derivedPathsFollowDataRoot();
    void ensureBaseDirectoriesCreatesTree();
    void ensureBaseDirectoriesReportsInvalidPath();
    void describeResolutionReportsSource();
};

void PathsTest::cleanup() {
    Paths::setDataRootOverride(QString());
    qunsetenv("NESVA_DATA_DIR");
}

void PathsTest::overrideWinsOverDefaults() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    qputenv("NESVA_DATA_DIR", "/tmp/should-be-ignored");

    Paths::setDataRootOverride(directory.path());
    QCOMPARE(Paths::dataRoot(), QDir::cleanPath(directory.path()));
}

void PathsTest::environmentVariableUsedWhenNoOverride() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Paths::setDataRootOverride(QString());
    qputenv("NESVA_DATA_DIR", directory.path().toUtf8());

    QCOMPARE(Paths::dataRoot(), QDir::cleanPath(directory.path()));
}

void PathsTest::derivedPathsFollowDataRoot() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Paths::setDataRootOverride(directory.path());

    const QString root = QDir::cleanPath(directory.path());
    QCOMPARE(Paths::databasePath(), QDir(root).absoluteFilePath(QStringLiteral("nesva.db")));
    QCOMPARE(Paths::logsDirectory(), QDir(root).absoluteFilePath(QStringLiteral("logs")));
    QCOMPARE(Paths::logFilePath(QStringLiteral("x.log")),
             QDir(root).absoluteFilePath(QStringLiteral("logs/x.log")));
    QCOMPARE(Paths::cacheDirectory(), QDir(root).absoluteFilePath(QStringLiteral("cache")));
}

void PathsTest::ensureBaseDirectoriesCreatesTree() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Paths::setDataRootOverride(directory.path());

    QVERIFY(Paths::ensureBaseDirectories().ok());
    QVERIFY(QFileInfo(Paths::dataRoot()).isDir());
    QVERIFY(QFileInfo(Paths::logsDirectory()).isDir());
    QVERIFY(QFileInfo(Paths::cacheDirectory()).isDir());

    // Repeated calls are no-ops and keep succeeding.
    QVERIFY(Paths::ensureBaseDirectories().ok());
}

void PathsTest::ensureBaseDirectoriesReportsInvalidPath() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileAsDirectory = directory.filePath(QStringLiteral("blocked"));
    QFile blocker(fileAsDirectory);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();

    Paths::setDataRootOverride(fileAsDirectory);
    const Status status = Paths::ensureBaseDirectories();
    QVERIFY(!status.ok());
    QCOMPARE(static_cast<int>(status.error().code()), static_cast<int>(ErrorCode::IoError));
}

void PathsTest::describeResolutionReportsSource() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Paths::setDataRootOverride(directory.path());
    QVERIFY(Paths::describeResolution().startsWith(QStringLiteral("override:")));

    Paths::setDataRootOverride(QString());
    qputenv("NESVA_DATA_DIR", directory.path().toUtf8());
    QVERIFY(Paths::describeResolution().startsWith(QStringLiteral("NESVA_DATA_DIR")));

    qunsetenv("NESVA_DATA_DIR");
    QVERIFY(Paths::describeResolution().startsWith(QStringLiteral("platform default:")));
}

QTEST_GUILESS_MAIN(PathsTest)
#include "tst_paths.moc"
