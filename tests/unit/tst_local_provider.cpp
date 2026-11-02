// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>

#include <memory>

#include "nesva/core/hashing.hpp"
#include "nesva/providers/local_provider.hpp"
#include "support/test_fixtures.hpp"

using namespace nesva;
using namespace nesva::providers;

class LocalProviderTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void uploadsAndDownloadsRoundTrip();
    void uploadRejectsPathTraversal();
    void uploadRejectsMissingSource();
    void uploadLeavesNoPartialFileOnCancellation();
    void fileExistsAndListing();
    void checksumMatchesContent();
    void testConnectionValidatesRoot();
    void ensureContainerCreatesDirectory();
    void handlesUnicodeAndSpaces();

private:
    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<LocalFilesystemProvider> provider_;
    QString sourceFile_;

    QString root() const { return directory_->filePath(QStringLiteral("archive")); }
};

void LocalProviderTest::init() {
    directory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(directory_->isValid());
    QVERIFY(QDir().mkpath(root()));
    provider_ = std::make_unique<LocalFilesystemProvider>(root());

    sourceFile_ = directory_->filePath(QStringLiteral("source.txt"));
    QByteArray content;
    for (int index = 0; index < 4096; ++index) {
        content.append(static_cast<char>('a' + (index % 26)));
    }
    QVERIFY(test::writeFile(sourceFile_, content));
}

void LocalProviderTest::cleanup() {
    provider_.reset();
    directory_.reset();
}

void LocalProviderTest::uploadsAndDownloadsRoundTrip() {
    const CancellationToken cancellation;
    TransferOptions options;

    const Result<TransferResult> uploaded =
        provider_->uploadFile(sourceFile_, QStringLiteral("docs/source.txt"), options, cancellation);
    QVERIFY(uploaded);
    QCOMPARE(uploaded.value().bytesTransferred, QFileInfo(sourceFile_).size());
    QVERIFY(!uploaded.value().sha256.isEmpty());
    QCOMPARE(uploaded.value().sha256, sha256File(sourceFile_).valueOr(QString()));

    const QString archived = QDir(root()).absoluteFilePath(QStringLiteral("docs/source.txt"));
    QVERIFY(QFileInfo::exists(archived));
    QCOMPARE(test::readFile(archived), test::readFile(sourceFile_));

    // Modification time is preserved so the archive keeps file identity.
    QCOMPARE(QFileInfo(archived).lastModified().toSecsSinceEpoch(),
             QFileInfo(sourceFile_).lastModified().toSecsSinceEpoch());

    const QString restored = directory_->filePath(QStringLiteral("restored.txt"));
    const Result<TransferResult> downloaded = provider_->downloadFile(
        QStringLiteral("docs/source.txt"), restored, options, cancellation);
    QVERIFY(downloaded);
    QCOMPARE(test::readFile(restored), test::readFile(sourceFile_));

    // Atomic writes: no temporary files left behind.
    const QStringList leftovers = QDir(QDir(root()).absoluteFilePath(QStringLiteral("docs")))
                                      .entryList(QDir::Files | QDir::Hidden | QDir::System);
    QCOMPARE(leftovers.size(), 1);
}

void LocalProviderTest::uploadRejectsPathTraversal() {
    const CancellationToken cancellation;
    const TransferOptions options;

    const Result<TransferResult> escape =
        provider_->uploadFile(sourceFile_, QStringLiteral("../outside.txt"), options, cancellation);
    QVERIFY(!escape);
    QCOMPARE(static_cast<int>(escape.error().code()), static_cast<int>(ErrorCode::InvalidArgument));

    const Result<TransferResult> absolute = provider_->uploadFile(
        sourceFile_, QStringLiteral("/etc/passwd"), options, cancellation);
    QVERIFY(!absolute);

    const QString outside = directory_->filePath(QStringLiteral("outside.txt"));
    QVERIFY(!QFileInfo::exists(outside));
}

void LocalProviderTest::uploadRejectsMissingSource() {
    const CancellationToken cancellation;
    const TransferOptions options;
    const Result<TransferResult> missing = provider_->uploadFile(
        directory_->filePath(QStringLiteral("does-not-exist")), QStringLiteral("x.txt"), options,
        cancellation);
    QVERIFY(!missing);
    QCOMPARE(static_cast<int>(missing.error().code()), static_cast<int>(ErrorCode::NotFound));
}

void LocalProviderTest::uploadLeavesNoPartialFileOnCancellation() {
    CancellationToken cancellation;
    cancellation.cancel();
    const TransferOptions options;

    const Result<TransferResult> uploaded =
        provider_->uploadFile(sourceFile_, QStringLiteral("cancelled.txt"), options, cancellation);
    QVERIFY(!uploaded);
    QCOMPARE(static_cast<int>(uploaded.error().code()), static_cast<int>(ErrorCode::Cancelled));
    QVERIFY(!QFileInfo::exists(QDir(root()).absoluteFilePath(QStringLiteral("cancelled.txt"))));
}

void LocalProviderTest::fileExistsAndListing() {
    const CancellationToken cancellation;
    const TransferOptions options;
    QVERIFY(provider_->uploadFile(sourceFile_, QStringLiteral("a/one.txt"), options, cancellation));
    QVERIFY(provider_->uploadFile(sourceFile_, QStringLiteral("b/two.txt"), options, cancellation));

    const Result<bool> exists = provider_->fileExists(QStringLiteral("a/one.txt"));
    QVERIFY(exists);
    QVERIFY(exists.value());

    const Result<bool> missing = provider_->fileExists(QStringLiteral("a/nope.txt"));
    QVERIFY(missing);
    QVERIFY(!missing.value());

    const Result<QVector<RemoteEntry>> listing = provider_->listFiles(QString(), true);
    QVERIFY(listing);
    QCOMPARE(listing.value().size(), 2);
    QStringList paths;
    for (const RemoteEntry& entry : listing.value()) {
        paths.append(entry.relativePath);
    }
    QVERIFY(paths.contains(QStringLiteral("a/one.txt")));
    QVERIFY(paths.contains(QStringLiteral("b/two.txt")));

    const Result<QVector<RemoteEntry>> shallow = provider_->listFiles(QStringLiteral("a"), false);
    QVERIFY(shallow);
    QCOMPARE(shallow.value().size(), 1);
}

void LocalProviderTest::checksumMatchesContent() {
    const CancellationToken cancellation;
    const TransferOptions options;
    QVERIFY(provider_->uploadFile(sourceFile_, QStringLiteral("hash.txt"), options, cancellation));

    const Result<QString> remote = provider_->checksum(QStringLiteral("hash.txt"));
    QVERIFY(remote);
    const Result<QString> local = sha256File(sourceFile_);
    QVERIFY(local);
    QCOMPARE(remote.value(), local.value());

    const Result<QString> missing = provider_->checksum(QStringLiteral("nope.txt"));
    QVERIFY(!missing);
    QCOMPARE(static_cast<int>(missing.error().code()), static_cast<int>(ErrorCode::NotFound));
}

void LocalProviderTest::testConnectionValidatesRoot() {
    QVERIFY(provider_->testConnection().ok());

    LocalFilesystemProvider missing(directory_->filePath(QStringLiteral("absent")));
    const Status status = missing.testConnection();
    QVERIFY(!status.ok());
    QCOMPARE(static_cast<int>(status.error().code()), static_cast<int>(ErrorCode::NotFound));

    LocalFilesystemProvider fileProvider(sourceFile_);
    const Status fileStatus = fileProvider.testConnection();
    QVERIFY(!fileStatus.ok());
    QCOMPARE(static_cast<int>(fileStatus.error().code()), static_cast<int>(ErrorCode::InvalidArgument));
}

void LocalProviderTest::ensureContainerCreatesDirectory() {
    const QString nested = directory_->filePath(QStringLiteral("nested/archive"));
    LocalFilesystemProvider provider(nested);
    QVERIFY(!QFileInfo::exists(nested));
    QVERIFY(provider.ensureContainer().ok());
    QVERIFY(QFileInfo(nested).isDir());
}

void LocalProviderTest::handlesUnicodeAndSpaces() {
    const CancellationToken cancellation;
    const TransferOptions options;
    const QString name = QStringLiteral("Dokumenti/poro\u010dilo 2026 \u00e6\u00f8\u00e5.txt");
    const Result<TransferResult> uploaded = provider_->uploadFile(sourceFile_, name, options, cancellation);
    QVERIFY2(uploaded.hasValue(), uploaded.hasValue() ? "" : uploaded.error().message().c_str());
    QVERIFY(QFileInfo::exists(QDir(root()).absoluteFilePath(name)));
}

QTEST_GUILESS_MAIN(LocalProviderTest)
#include "tst_local_provider.moc"
