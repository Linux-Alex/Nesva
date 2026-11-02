// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QDateTime>
#include <QtCore/QUuid>

#include <memory>

#include "nesva/security/secret_store.hpp"

using namespace nesva;
using namespace nesva::security;

class SecretStoreTest : public QObject {
    Q_OBJECT

private slots:
    void createsABackend();
    void roundTripWhenAvailable();
    void missingSecretIsNotFound();
    void removeIsIdempotent();
    void referenceHelpers();

private:
    static bool backendUsable(SecretStore& store, QString* why) {
        if (!store.isAvailable()) {
            *why = store.unavailableReason();
            return false;
        }
        // A locked or prompting keyring is a valid environment, just not one a
        // headless test can use.
        const Result<QByteArray> probe =
            store.lookup(QStringLiteral("nesva-test"), QStringLiteral("probe"));
        if (!probe && probe.error().code() != ErrorCode::NotFound) {
            *why = QString::fromStdString(probe.error().toDisplayString());
            return false;
        }
        return true;
    }
};

void SecretStoreTest::createsABackend() {
    const std::unique_ptr<SecretStore> store = createSecretStore();
    QVERIFY(store != nullptr);
    QVERIFY(!store->backendName().isEmpty());
    qInfo("credential backend: %s (available=%d)",
          qPrintable(store->backendName()), store->isAvailable());
}

void SecretStoreTest::roundTripWhenAvailable() {
    const std::unique_ptr<SecretStore> store = createSecretStore();
    QString why;
    if (!backendUsable(*store, &why)) {
        QSKIP(qPrintable(QStringLiteral("credential store not usable here: %1").arg(why)));
    }

    const QString service = QStringLiteral("nesva-test");
    const QString account = QStringLiteral("unit-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QByteArray secret = QByteArray("s3cr3t-value-") + QByteArray::number(QDateTime::currentMSecsSinceEpoch());

    const Status stored = store->store(service, account, secret);
    QVERIFY2(stored.ok(), stored.ok() ? "" : stored.error().toDisplayString().c_str());

    const Result<QByteArray> looked = store->lookup(service, account);
    QVERIFY2(looked.hasValue(), looked.hasValue() ? "" : looked.error().toDisplayString().c_str());
    QCOMPARE(looked.value(), secret);

    // Overwriting must replace, not duplicate.
    const QByteArray updated("updated-value");
    QVERIFY(store->store(service, account, updated).ok());
    const Result<QByteArray> reloaded = store->lookup(service, account);
    QVERIFY(reloaded.hasValue());
    QCOMPARE(reloaded.value(), updated);

    QVERIFY(store->remove(service, account).ok());
    const Result<QByteArray> gone = store->lookup(service, account);
    QVERIFY(!gone);
    QCOMPARE(static_cast<int>(gone.error().code()), static_cast<int>(ErrorCode::NotFound));
}

void SecretStoreTest::missingSecretIsNotFound() {
    const std::unique_ptr<SecretStore> store = createSecretStore();
    QString why;
    if (!backendUsable(*store, &why)) {
        QSKIP(qPrintable(QStringLiteral("credential store not usable here: %1").arg(why)));
    }

    const Result<QByteArray> missing = store->lookup(
        QStringLiteral("nesva-test"),
        QStringLiteral("absent-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY(!missing);
    QCOMPARE(static_cast<int>(missing.error().code()), static_cast<int>(ErrorCode::NotFound));
}

void SecretStoreTest::removeIsIdempotent() {
    const std::unique_ptr<SecretStore> store = createSecretStore();
    QString why;
    if (!backendUsable(*store, &why)) {
        QSKIP(qPrintable(QStringLiteral("credential store not usable here: %1").arg(why)));
    }
    // Removing something that was never stored must succeed.
    QVERIFY(store->remove(QStringLiteral("nesva-test"),
                          QStringLiteral("never-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
                .ok());
}

void SecretStoreTest::referenceHelpers() {
    const QString reference = secretReference(QStringLiteral("restic"), QStringLiteral("repo-7"));
    QCOMPARE(reference, QStringLiteral("restic/repo-7"));

    QString service;
    QString account;
    QVERIFY(parseSecretReference(reference, &service, &account));
    QCOMPARE(service, QStringLiteral("restic"));
    QCOMPARE(account, QStringLiteral("repo-7"));

    QVERIFY(!parseSecretReference(QStringLiteral("noref"), &service, &account));
    QVERIFY(!parseSecretReference(QStringLiteral("/leading"), &service, &account));
    QVERIFY(!parseSecretReference(QStringLiteral("trailing/"), &service, &account));
}

QTEST_GUILESS_MAIN(SecretStoreTest)
#include "tst_secret_store.moc"
