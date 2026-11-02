// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QTemporaryDir>

#include <memory>

#include "nesva/core/cancellation.hpp"
#include "nesva/scan/file_scanner.hpp"
#include "support/test_fixtures.hpp"

using namespace nesva;
using namespace nesva::scan;

namespace {

struct Tree {
    std::unique_ptr<QTemporaryDir> directory;

    Tree() : directory(std::make_unique<QTemporaryDir>()) {}
    QString path(const QString& relative) const { return directory->filePath(relative); }
    void make() {
        QVERIFY(directory->isValid());
        QVERIFY(test::writeFile(path(QStringLiteral("a.txt")), "alpha"));
        QVERIFY(test::writeFile(path(QStringLiteral("docs/b.txt")), "bravo"));
        QVERIFY(test::writeFile(path(QStringLiteral("docs/nested/c.txt")), "charlie"));
        QVERIFY(test::writeFile(path(QStringLiteral("docs/cache/ignore.bin")), "junk"));
        QVERIFY(test::writeFile(path(QStringLiteral(".hidden")), "hidden"));
    }
};

} // namespace

class FileScannerTest : public QObject {
    Q_OBJECT

private slots:
    void scansRegularFiles();
    void excludesByGlobPattern();
    void skipsExcludedRoots();
    void neverFollowsSymlinksByDefault();
    void reportsMissingSource();
    void reportsNonDirectorySource();
    void honoursDepthLimit();
    void stopsWhenCancelled();
    void handlesSpacesAndUnicode();
    void recordsSkipsWithReasons();
};

void FileScannerTest::scansRegularFiles() {
    Tree tree;
    tree.make();
    ScanOptions options;
    options.followSymlinks = false;
    const CancellationToken cancellation;

    const Result<ScanResult> result =
        scanSources({{tree.directory->path(), QStringLiteral("tree")}}, options, cancellation);
    QVERIFY(result);
    // a.txt, .hidden, docs/b.txt, docs/c.txt, docs/cache/ignore.bin
    QCOMPARE(result.value().fileCount(), 5);
    QVERIFY(result.value().totalBytes > 0);

    QStringList paths;
    for (const ScanEntry& entry : result.value().files) {
        paths.append(entry.relativePath);
    }
    QVERIFY(paths.contains(QStringLiteral("a.txt")));
    QVERIFY(paths.contains(QStringLiteral("docs/nested/c.txt")));
    QVERIFY(!result.value().cancelled);
}

void FileScannerTest::excludesByGlobPattern() {
    Tree tree;
    tree.make();
    ScanOptions options;
    const Result<ExclusionRules> rules =
        ExclusionRules::compile({{QStringLiteral("*.bin"), PatternKind::Glob}});
    QVERIFY(rules);
    options.exclusions = rules.value();
    const CancellationToken cancellation;

    const Result<ScanResult> result =
        scanSources({{tree.directory->path(), QStringLiteral("tree")}}, options, cancellation);
    QVERIFY(result);
    QCOMPARE(result.value().fileCount(), 4);
    bool foundExclusionReason = false;
    for (const ScanSkip& skip : result.value().skipped) {
        if (skip.path.endsWith(QStringLiteral("ignore.bin"))) {
            foundExclusionReason = skip.reason.contains(QStringLiteral("excluded"));
        }
    }
    QVERIFY(foundExclusionReason);
}

void FileScannerTest::skipsExcludedRoots() {
    Tree tree;
    tree.make();
    ScanOptions options;
    options.excludedRoots.append(tree.path(QStringLiteral("docs/cache")));
    const CancellationToken cancellation;

    const Result<ScanResult> result =
        scanSources({{tree.directory->path(), QStringLiteral("tree")}}, options, cancellation);
    QVERIFY(result);
    QCOMPARE(result.value().fileCount(), 4);
    for (const ScanEntry& entry : result.value().files) {
        QVERIFY(!entry.relativePath.contains(QStringLiteral("cache")));
    }
    QVERIFY(!result.value().skipped.isEmpty());
}

void FileScannerTest::neverFollowsSymlinksByDefault() {
#ifdef Q_OS_UNIX
    Tree tree;
    tree.make();
    const QString linkPath = tree.path(QStringLiteral("link-to-docs"));
    QVERIFY(QFile::link(tree.path(QStringLiteral("docs")), linkPath) || true);

    ScanOptions options;
    options.followSymlinks = false;
    const CancellationToken cancellation;
    const Result<ScanResult> result =
        scanSources({{tree.directory->path(), QStringLiteral("tree")}}, options, cancellation);
    QVERIFY(result);
    for (const ScanEntry& entry : result.value().files) {
        QVERIFY(!entry.relativePath.startsWith(QStringLiteral("link-to-docs")));
    }

    bool linkReported = false;
    for (const ScanSkip& skip : result.value().skipped) {
        if (skip.reason.contains(QStringLiteral("symlink"))) {
            linkReported = true;
        }
    }
    QVERIFY(linkReported);
#else
    QSKIP("symlink test requires a POSIX filesystem");
#endif
}

void FileScannerTest::reportsMissingSource() {
    ScanOptions options;
    const CancellationToken cancellation;
    const Result<ScanResult> result = scanSources(
        {{QStringLiteral("/nonexistent/nesva-source"), QStringLiteral("x")}}, options, cancellation);
    QVERIFY(!result);
    QCOMPARE(static_cast<int>(result.error().code()), static_cast<int>(ErrorCode::NotFound));
}

void FileScannerTest::reportsNonDirectorySource() {
    Tree tree;
    tree.make();
    ScanOptions options;
    const CancellationToken cancellation;
    const Result<ScanResult> result = scanSources(
        {{tree.path(QStringLiteral("a.txt")), QStringLiteral("x")}}, options, cancellation);
    QVERIFY(!result);
    QCOMPARE(static_cast<int>(result.error().code()), static_cast<int>(ErrorCode::InvalidArgument));
}

void FileScannerTest::honoursDepthLimit() {
    Tree tree;
    QVERIFY(tree.directory->isValid());
    QVERIFY(test::writeFile(tree.path(QStringLiteral("d1/d2/d3/deep.txt")), "deep"));

    ScanOptions options;
    options.maxDepth = 1;
    const CancellationToken cancellation;
    const Result<ScanResult> result =
        scanSources({{tree.directory->path(), QStringLiteral("tree")}}, options, cancellation);
    QVERIFY(result);
    QCOMPARE(result.value().fileCount(), 0);
    bool depthReason = false;
    for (const ScanSkip& skip : result.value().skipped) {
        if (skip.reason.contains(QStringLiteral("depth"))) {
            depthReason = true;
        }
    }
    QVERIFY(depthReason);
}

void FileScannerTest::stopsWhenCancelled() {
    Tree tree;
    tree.make();
    ScanOptions options;
    CancellationToken cancellation;
    cancellation.cancel();

    const Result<ScanResult> result =
        scanSources({{tree.directory->path(), QStringLiteral("tree")}}, options, cancellation);
    QVERIFY(result);
    QVERIFY(result.value().cancelled);
}

void FileScannerTest::handlesSpacesAndUnicode() {
    Tree tree;
    QVERIFY(tree.directory->isValid());
    QVERIFY(test::writeFile(tree.path(QStringLiteral("Dokumenti \u00e6\u00f8\u00e5/poro\u010dilo 2026.txt")),
                            "vsebina"));
    ScanOptions options;
    const CancellationToken cancellation;
    const Result<ScanResult> result =
        scanSources({{tree.directory->path(), QStringLiteral("tree")}}, options, cancellation);
    QVERIFY(result);
    QCOMPARE(result.value().fileCount(), 1);
    QCOMPARE(result.value().files.constFirst().relativePath,
             QStringLiteral("Dokumenti \u00e6\u00f8\u00e5/poro\u010dilo 2026.txt"));
}

void FileScannerTest::recordsSkipsWithReasons() {
    Tree tree;
    tree.make();
    ScanOptions options;
    options.includeHidden = false;
    const CancellationToken cancellation;
    const Result<ScanResult> result =
        scanSources({{tree.directory->path(), QStringLiteral("tree")}}, options, cancellation);
    QVERIFY(result);
    bool hiddenReported = false;
    for (const ScanSkip& skip : result.value().skipped) {
        if (skip.path.endsWith(QStringLiteral(".hidden"))) {
            hiddenReported = skip.reason.contains(QStringLiteral("hidden"));
        }
    }
    QVERIFY(hiddenReported);
}

QTEST_GUILESS_MAIN(FileScannerTest)
#include "tst_file_scanner.moc"
