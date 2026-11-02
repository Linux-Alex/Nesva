// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QJsonObject>

#include "nesva/core/redactor.hpp"

using namespace nesva;

class RedactorTest : public QObject {
    Q_OBJECT

private slots:
    void masksRegisteredSecrets();
    void ignoresShortSecrets();
    void masksKeyValueAssignments();
    void masksBearerTokens();
    void masksJwtShapedValues();
    void masksAwsAccessKeys();
    void masksPrivateKeyBlocks();
    void masksNestedJsonObjects();
    void flagsSensitiveKeyNames();
    void leavesOrdinaryTextIntact();
};

void RedactorTest::masksRegisteredSecrets() {
    Redactor redactor;
    redactor.addSecret(QStringLiteral("sup3rS3cretToken"));
    QCOMPARE(redactor.secretCount(), 1);

    const QString redacted =
        redactor.redact(QStringLiteral("uploading with sup3rS3cretToken now"));
    QVERIFY(!redacted.contains(QStringLiteral("sup3rS3cretToken")));
    QVERIFY(redacted.contains(Redactor::mask()));
    QVERIFY(redacted.startsWith(QStringLiteral("uploading with ")));
}

void RedactorTest::ignoresShortSecrets() {
    Redactor redactor;
    redactor.addSecret(QStringLiteral("ab"));
    QCOMPARE(redactor.secretCount(), 0);
    QCOMPARE(redactor.redact(QStringLiteral("ab is a short word")),
             QStringLiteral("ab is a short word"));
}

void RedactorTest::masksKeyValueAssignments() {
    Redactor redactor;
    const QString redacted =
        redactor.redact(QStringLiteral("rclone config password=hunter2 user=me"));
    QVERIFY(!redacted.contains(QStringLiteral("hunter2")));
    QVERIFY(redacted.contains(QStringLiteral("password=")));

    const QString quoted = redactor.redact(QStringLiteral("refresh_token: \"abc123def456\""));
    QVERIFY(!quoted.contains(QStringLiteral("abc123def456")));
}

void RedactorTest::masksBearerTokens() {
    Redactor redactor;
    const QString redacted = redactor.redact(
        QStringLiteral("Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload.sig"));
    QVERIFY(!redacted.contains(QStringLiteral("eyJhbGciOiJIUzI1NiJ9")));
    QVERIFY(!redacted.contains(QStringLiteral("payload.sig")));
}

void RedactorTest::masksJwtShapedValues() {
    Redactor redactor;
    const QString token = QStringLiteral("eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiJ9.eyJzdWIiOiIxMjM0In0."
                                        "c2lnbmF0dXJlLXZhbHVl");
    const QString redacted = redactor.redact(QStringLiteral("token: %1").arg(token));
    QVERIFY(!redacted.contains(token));
}

void RedactorTest::masksAwsAccessKeys() {
    Redactor redactor;
    const QString redacted = redactor.redact(QStringLiteral("key AKIAIOSFODNN7EXAMPLE here"));
    QVERIFY(!redacted.contains(QStringLiteral("AKIAIOSFODNN7EXAMPLE")));
}

void RedactorTest::masksPrivateKeyBlocks() {
    Redactor redactor;
    const QString pem = QStringLiteral(
        "-----BEGIN RSA PRIVATE KEY-----\nMIIEpAIBAAKCAQEA1234\n-----END RSA PRIVATE KEY-----");
    const QString redacted = redactor.redact(pem);
    QVERIFY(!redacted.contains(QStringLiteral("MIIEpAIBAAKCAQEA1234")));
    QVERIFY(redacted.contains(Redactor::mask()));
}

void RedactorTest::masksNestedJsonObjects() {
    Redactor redactor;
    QJsonObject nested{{QStringLiteral("password"), QStringLiteral("hunter2")},
                       {QStringLiteral("note"), QStringLiteral("token=abc123456")}};
    QJsonObject root{{QStringLiteral("storage"), nested},
                     {QStringLiteral("name"), QStringLiteral("iCloud")}};

    const QJsonObject redacted = redactor.redactObject(root);
    QCOMPARE(redacted.value(QStringLiteral("name")).toString(), QStringLiteral("iCloud"));

    const QJsonObject storage = redacted.value(QStringLiteral("storage")).toObject();
    QCOMPARE(storage.value(QStringLiteral("password")).toString(), Redactor::mask());
    QVERIFY(!storage.value(QStringLiteral("note")).toString().contains(QStringLiteral("abc123456")));
}

void RedactorTest::flagsSensitiveKeyNames() {
    QVERIFY(isSensitiveKeyName(QStringLiteral("password")));
    QVERIFY(isSensitiveKeyName(QStringLiteral("refresh_token")));
    QVERIFY(isSensitiveKeyName(QStringLiteral("ApiKey")));
    QVERIFY(isSensitiveKeyName(QStringLiteral("client_secret")));
    QVERIFY(!isSensitiveKeyName(QStringLiteral("username")));
    QVERIFY(!isSensitiveKeyName(QStringLiteral("remote_path")));
    QVERIFY(!isSensitiveKeyName(QString()));
}

void RedactorTest::leavesOrdinaryTextIntact() {
    Redactor redactor;
    const QString text = QStringLiteral("copied 12 files (3.4 MiB) to icloud:Nesva/2026");
    QCOMPARE(redactor.redact(text), text);
}

QTEST_GUILESS_MAIN(RedactorTest)
#include "tst_redactor.moc"
