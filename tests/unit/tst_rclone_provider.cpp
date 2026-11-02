// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>

#include <functional>
#include <memory>

#include "nesva/providers/rclone_provider.hpp"
#include "support/test_fixtures.hpp"

using namespace nesva;
using namespace nesva::providers;

namespace {

/// Records every invocation and returns a scripted outcome. This exercises the
/// real provider logic (argument construction, parsing, retries, error mapping)
/// without a cloud account and without pretending a transfer happened.
class FakeRunner final : public ExternalRunner {
public:
    struct Call {
        QString program;
        QStringList arguments;
    };

    QVector<Call> calls;
    std::function<ProcessOutcome(const QStringList&, int callIndex)> responder;

    ProcessOutcome run(const QString& program, const QStringList& arguments,
                       const CancellationToken& cancellation, int timeoutMs) override {
        Q_UNUSED(cancellation);
        Q_UNUSED(timeoutMs);
        calls.append({program, arguments});
        if (responder) {
            return responder(arguments, static_cast<int>(calls.size()) - 1);
        }
        return ok();
    }

    static ProcessOutcome ok(const QByteArray& output = {}) {
        ProcessOutcome outcome;
        outcome.exitCode = 0;
        outcome.standardOutput = output;
        return outcome;
    }

    static ProcessOutcome fail(int code, const QByteArray& stderrText) {
        ProcessOutcome outcome;
        outcome.exitCode = code;
        outcome.standardError = stderrText;
        return outcome;
    }
};

} // namespace

class RcloneProviderTest : public QObject {
    Q_OBJECT

private slots:
    void init();

    void buildsUploadArgumentsWithoutShell();
    void keepsUnicodeAndSpacesAsSingleArguments();
    void uploadsThroughCopyto();
    void mapsFailureToErrorWithStderr();
    void retriesThenSucceeds();
    void stopsRetryingAndFails();
    void honoursCancellation();
    void fileExistsDistinguishesMissing();
    void testConnectionUsesLsd();
    void ensureContainerUsesMkdir();
    void parsesLsjsonListing();
    void rejectsInvalidJson();
    void listsRecursively();
    void remoteTargetRejectsTraversal();

private:
    std::shared_ptr<FakeRunner> runner_;
    std::unique_ptr<RcloneProvider> provider_;
    std::unique_ptr<QTemporaryDir> directory_;
    QString sourceFile_;
};

void RcloneProviderTest::init() {
    runner_ = std::make_shared<FakeRunner>();
    provider_ = std::make_unique<RcloneProvider>(QStringLiteral("icloud"), QStringLiteral("Nesva/Archive"),
                                                 QStringLiteral("rclone"), runner_);
    directory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(directory_->isValid());
    sourceFile_ = directory_->filePath(QStringLiteral("file one.txt"));
    QVERIFY(test::writeFile(sourceFile_, QByteArray(2048, 'x')));
}

void RcloneProviderTest::buildsUploadArgumentsWithoutShell() {
    TransferOptions options;
    options.bandwidthLimitKiB = 256;
    options.verifyChecksum = true;

    const QStringList arguments = RcloneProvider::uploadArguments(
        QStringLiteral("/tmp/a.txt"), QStringLiteral("icloud:Nesva/Archive/a.txt"), options);

    QCOMPARE(arguments.constFirst(), QStringLiteral("copyto"));
    QCOMPARE(arguments.at(1), QStringLiteral("/tmp/a.txt"));
    QCOMPARE(arguments.at(2), QStringLiteral("icloud:Nesva/Archive/a.txt"));
    QVERIFY(arguments.contains(QStringLiteral("--bwlimit")));
    QVERIFY(arguments.contains(QStringLiteral("256k")));
    QVERIFY(arguments.contains(QStringLiteral("--checksum")));
    // No shell interpolation: there is no "sh", "-c" or combined command string.
    QVERIFY(!arguments.contains(QStringLiteral("-c")));
    QVERIFY(!arguments.contains(QStringLiteral("sh")));
    for (const QString& argument : arguments) {
        QVERIFY(!argument.contains(QStringLiteral("&&")));
        QVERIFY(!argument.contains(QLatin1Char('|')));
    }
}

void RcloneProviderTest::keepsUnicodeAndSpacesAsSingleArguments() {
    const TransferOptions options;
    const QStringList arguments = RcloneProvider::uploadArguments(
        sourceFile_, QStringLiteral("icloud:Nesva/Archive/Dokumenti/poro\u010dilo 2026.txt"), options);
    QCOMPARE(arguments.at(1), sourceFile_);
    QCOMPARE(arguments.at(2), QStringLiteral("icloud:Nesva/Archive/Dokumenti/poro\u010dilo 2026.txt"));
}

void RcloneProviderTest::uploadsThroughCopyto() {
    const CancellationToken cancellation;
    const TransferOptions options;
    const Result<TransferResult> uploaded =
        provider_->uploadFile(sourceFile_, QStringLiteral("docs/file one.txt"), options, cancellation);
    QVERIFY(uploaded);
    QCOMPARE(uploaded.value().bytesTransferred, QFileInfo(sourceFile_).size());
    // rclone cannot report a hash for every backend.
    QVERIFY(uploaded.value().sha256.isEmpty());

    QCOMPARE(runner_->calls.size(), 1);
    QCOMPARE(runner_->calls.constFirst().program, QStringLiteral("rclone"));
    QCOMPARE(runner_->calls.constFirst().arguments.at(2),
             QStringLiteral("icloud:Nesva/Archive/docs/file one.txt"));
}

void RcloneProviderTest::mapsFailureToErrorWithStderr() {
    runner_->responder = [](const QStringList&, int) {
        return FakeRunner::fail(3, "ERROR : directory not found");
    };
    const CancellationToken cancellation;
    const TransferOptions options;
    const Result<TransferResult> uploaded =
        provider_->uploadFile(sourceFile_, QStringLiteral("x.txt"), options, cancellation);
    QVERIFY(!uploaded);
    QCOMPARE(static_cast<int>(uploaded.error().code()), static_cast<int>(ErrorCode::ExternalToolFailed));
    QVERIFY(uploaded.error().detail().find("directory not found") != std::string::npos);
}

void RcloneProviderTest::retriesThenSucceeds() {
    runner_->responder = [](const QStringList&, int index) {
        return index < 2 ? FakeRunner::fail(1, "temporary failure") : FakeRunner::ok();
    };
    const CancellationToken cancellation;
    TransferOptions options;
    options.maxAttempts = 3;
    options.retryBackoffMs = 0;

    const Result<TransferResult> uploaded =
        provider_->uploadFile(sourceFile_, QStringLiteral("x.txt"), options, cancellation);
    QVERIFY(uploaded);
    QCOMPARE(runner_->calls.size(), 3);
}

void RcloneProviderTest::stopsRetryingAndFails() {
    runner_->responder = [](const QStringList&, int) { return FakeRunner::fail(1, "always fails"); };
    const CancellationToken cancellation;
    TransferOptions options;
    options.maxAttempts = 2;
    options.retryBackoffMs = 0;

    const Result<TransferResult> uploaded =
        provider_->uploadFile(sourceFile_, QStringLiteral("x.txt"), options, cancellation);
    QVERIFY(!uploaded);
    QCOMPARE(runner_->calls.size(), 2);
    QCOMPARE(static_cast<int>(uploaded.error().code()), static_cast<int>(ErrorCode::ExternalToolFailed));
}

void RcloneProviderTest::honoursCancellation() {
    CancellationToken cancellation;
    cancellation.cancel();
    const TransferOptions options;
    const Result<TransferResult> uploaded =
        provider_->uploadFile(sourceFile_, QStringLiteral("x.txt"), options, cancellation);
    QVERIFY(!uploaded);
    QCOMPARE(static_cast<int>(uploaded.error().code()), static_cast<int>(ErrorCode::Cancelled));
    QCOMPARE(runner_->calls.size(), 0);
}

void RcloneProviderTest::fileExistsDistinguishesMissing() {
    runner_->responder = [](const QStringList&, int) { return FakeRunner::ok("{}"); };
    const Result<bool> exists = provider_->fileExists(QStringLiteral("a.txt"));
    QVERIFY(exists);
    QVERIFY(exists.value());

    runner_->calls.clear();
    runner_->responder = [](const QStringList&, int) {
        return FakeRunner::fail(1, "ERROR : directory not found");
    };
    const Result<bool> missing = provider_->fileExists(QStringLiteral("a.txt"));
    QVERIFY(missing);
    QVERIFY(!missing.value());

    runner_->calls.clear();
    runner_->responder = [](const QStringList&, int) {
        return FakeRunner::fail(1, "ERROR : authentication failed");
    };
    const Result<bool> error = provider_->fileExists(QStringLiteral("a.txt"));
    QVERIFY(!error);
    QCOMPARE(static_cast<int>(error.error().code()), static_cast<int>(ErrorCode::ExternalToolFailed));
}

void RcloneProviderTest::testConnectionUsesLsd() {
    QVERIFY(provider_->testConnection().ok());
    QCOMPARE(runner_->calls.constFirst().arguments.constFirst(), QStringLiteral("lsd"));
    QCOMPARE(runner_->calls.constFirst().arguments.at(1), QStringLiteral("icloud:"));

    runner_->calls.clear();
    runner_->responder = [](const QStringList&, int) {
        return FakeRunner::fail(1, "ERROR : failed to authenticate");
    };
    const Status status = provider_->testConnection();
    QVERIFY(!status.ok());
    QVERIFY(QString::fromStdString(status.error().detail()).contains(QStringLiteral("authenticate")));
}

void RcloneProviderTest::ensureContainerUsesMkdir() {
    QVERIFY(provider_->ensureContainer().ok());
    QCOMPARE(runner_->calls.constFirst().arguments.constFirst(), QStringLiteral("mkdir"));
    QCOMPARE(runner_->calls.constFirst().arguments.at(1), QStringLiteral("icloud:Nesva/Archive"));
}

void RcloneProviderTest::parsesLsjsonListing() {
    const QByteArray json = R"([
        {"Path":"a.txt","Name":"a.txt","Size":12,"ModTime":"2026-01-01T10:00:00.000000000Z","IsDir":false},
        {"Path":"sub","Name":"sub","Size":-1,"IsDir":true},
        {"Path":"sub/b.txt","Name":"b.txt","Size":34,"ModTime":"2026-01-02T11:30:00Z","IsDir":false}
    ])";
    const Result<QVector<RemoteEntry>> parsed = RcloneProvider::parseListing(json);
    QVERIFY(parsed);
    QCOMPARE(parsed.value().size(), 3);
    QCOMPARE(parsed.value().at(0).relativePath, QStringLiteral("a.txt"));
    QCOMPARE(parsed.value().at(0).sizeBytes, qint64(12));
    QVERIFY(!parsed.value().at(0).isDirectory);
    QVERIFY(parsed.value().at(1).isDirectory);
    QCOMPARE(parsed.value().at(2).sizeBytes, qint64(34));
}

void RcloneProviderTest::rejectsInvalidJson() {
    const Result<QVector<RemoteEntry>> parsed = RcloneProvider::parseListing("not json");
    QVERIFY(!parsed);
    QCOMPARE(static_cast<int>(parsed.error().code()), static_cast<int>(ErrorCode::ExternalToolFailed));

    const Result<QVector<RemoteEntry>> object = RcloneProvider::parseListing(R"({"Path":"x"})");
    QVERIFY(!object);
}

void RcloneProviderTest::listsRecursively() {
    runner_->responder = [](const QStringList&, int) {
        return FakeRunner::ok(R"([{"Path":"d/e.txt","Size":1,"IsDir":false}])");
    };
    const Result<QVector<RemoteEntry>> entries = provider_->listFiles(QString(), true);
    QVERIFY(entries);
    QCOMPARE(entries.value().size(), 1);
    QVERIFY(runner_->calls.constFirst().arguments.contains(QStringLiteral("--recursive")));
    QCOMPARE(runner_->calls.constFirst().arguments.at(1), QStringLiteral("icloud:Nesva/Archive"));
}

void RcloneProviderTest::remoteTargetRejectsTraversal() {
    QVERIFY(!provider_->remoteTargetFor(QStringLiteral("../escape")).hasValue());
    QVERIFY(!provider_->remoteTargetFor(QStringLiteral("/absolute")).hasValue());
    const Result<QString> valid = provider_->remoteTargetFor(QStringLiteral("a/b.txt"));
    QVERIFY(valid);
    QCOMPARE(valid.value(), QStringLiteral("icloud:Nesva/Archive/a/b.txt"));
}

QTEST_GUILESS_MAIN(RcloneProviderTest)
#include "tst_rclone_provider.moc"
