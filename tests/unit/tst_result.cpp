// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <string>

#include "nesva/core/result.hpp"

using namespace nesva;

namespace {
Result<int> produceValue() {
    return 42;
}

Result<int> produceError() {
    return Error(ErrorCode::NotFound, "missing", "detail-text");
}

Status produceStatus(bool ok) {
    if (ok) {
        return Status();
    }
    return makeError(ErrorCode::IoError, "io failed");
}
} // namespace

class ResultTest : public QObject {
    Q_OBJECT

private slots:
    void valueRoundTrip();
    void errorRoundTrip();
    void implicitConstruction();
    void valueOrFallsBack();
    void takeMovesValue();
    void statusDefaultsToOk();
    void statusCarriesError();
    void errorCodeTokensAreStable();
    void displayStringFormatting();
};

void ResultTest::valueRoundTrip() {
    const Result<int> result = produceValue();
    QVERIFY(result.hasValue());
    QVERIFY(static_cast<bool>(result));
    QCOMPARE(result.value(), 42);
    QCOMPARE(*result, 42);
}

void ResultTest::errorRoundTrip() {
    const Result<int> result = produceError();
    QVERIFY(!result.hasValue());
    QVERIFY(!static_cast<bool>(result));
    QCOMPARE(static_cast<int>(result.error().code()), static_cast<int>(ErrorCode::NotFound));
    QCOMPARE(QString::fromStdString(result.error().message()), QStringLiteral("missing"));
    QCOMPARE(QString::fromStdString(result.error().detail()), QStringLiteral("detail-text"));
}

void ResultTest::implicitConstruction() {
    const Result<std::string> text = std::string("value");
    QVERIFY(text);
    QCOMPARE(QString::fromStdString(text.value()), QStringLiteral("value"));

    const Result<std::string> failure = Error(ErrorCode::InvalidArgument, "bad");
    QVERIFY(!failure);
    QCOMPARE(static_cast<int>(failure.error().code()), static_cast<int>(ErrorCode::InvalidArgument));
}

void ResultTest::valueOrFallsBack() {
    const Result<int> value = 7;
    QCOMPARE(value.valueOr(1), 7);

    const Result<int> failure = Error(ErrorCode::Internal, "boom");
    QCOMPARE(failure.valueOr(1), 1);
}

void ResultTest::takeMovesValue() {
    Result<std::string> text = std::string("payload");
    const std::string moved = std::move(text).take();
    QCOMPARE(QString::fromStdString(moved), QStringLiteral("payload"));
}

void ResultTest::statusDefaultsToOk() {
    const Status status = produceStatus(true);
    QVERIFY(status.ok());
    QVERIFY(static_cast<bool>(status));
}

void ResultTest::statusCarriesError() {
    const Status status = produceStatus(false);
    QVERIFY(!status.ok());
    QCOMPARE(static_cast<int>(status.error().code()), static_cast<int>(ErrorCode::IoError));
    QCOMPARE(QString::fromStdString(status.error().message()), QStringLiteral("io failed"));
}

void ResultTest::errorCodeTokensAreStable() {
    // The numeric values are part of the JSON/automation contract.
    QCOMPARE(static_cast<int>(ErrorCode::Ok), 0);
    QCOMPARE(static_cast<int>(ErrorCode::NotFound), 2);
    QCOMPARE(static_cast<int>(ErrorCode::NotImplemented), 12);
    QCOMPARE(QString::fromUtf8(toToken(ErrorCode::PermissionDenied)),
             QStringLiteral("permission_denied"));
    QCOMPARE(QString::fromUtf8(toToken(ErrorCode::NotImplemented)),
             QStringLiteral("not_implemented"));
}

void ResultTest::displayStringFormatting() {
    const Error withDetail(ErrorCode::IoError, "cannot write", "/tmp/x");
    QCOMPARE(QString::fromStdString(withDetail.toDisplayString()), QStringLiteral("cannot write (/tmp/x)"));

    const Error withoutDetail(ErrorCode::IoError, "cannot write");
    QCOMPARE(QString::fromStdString(withoutDetail.toDisplayString()), QStringLiteral("cannot write"));
}

QTEST_GUILESS_MAIN(ResultTest)
#include "tst_result.moc"
