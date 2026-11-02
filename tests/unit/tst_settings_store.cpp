// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QTemporaryDir>

#include "nesva/config/settings_store.hpp"
#include "nesva/db/database.hpp"

using namespace nesva;
using nesva::config::SettingsStore;
using nesva::db::Database;

class SettingsStoreTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void returnsDefaultsForMissingKeys();
    void storesAndReadsStrings();
    void storesAndReadsIntegers();
    void storesAndReadsBooleans();
    void overwritesExistingKey();
    void persistsAcrossReopen();
    void containsAndRemove();
    void rejectsEmptyKey();
    void listsAllSettings();
    void handlesUnicodeValues();

private:
    QTemporaryDir* directory_ = nullptr;
    Database* database_ = nullptr;
    SettingsStore* settings_ = nullptr;
};

void SettingsStoreTest::init() {
    directory_ = new QTemporaryDir();
    QVERIFY(directory_->isValid());
    database_ = new Database();
    QVERIFY(database_->open(directory_->filePath(QStringLiteral("nesva.db"))).ok());
    QVERIFY(database_->migrate().ok());
    settings_ = new SettingsStore(*database_);
}

void SettingsStoreTest::cleanup() {
    delete settings_;
    delete database_;
    delete directory_;
    settings_ = nullptr;
    database_ = nullptr;
    directory_ = nullptr;
}

void SettingsStoreTest::returnsDefaultsForMissingKeys() {
    const Result<QString> text = settings_->getString(QStringLiteral("missing.key"), QStringLiteral("fallback"));
    QVERIFY(text);
    QCOMPARE(text.value(), QStringLiteral("fallback"));

    const Result<qint64> number = settings_->getInteger(QStringLiteral("missing.number"), 17);
    QVERIFY(number);
    QCOMPARE(number.value(), qint64(17));

    const Result<bool> flag = settings_->getBoolean(QStringLiteral("missing.flag"), true);
    QVERIFY(flag);
    QVERIFY(flag.value());
}

void SettingsStoreTest::storesAndReadsStrings() {
    QVERIFY(settings_->setString(QStringLiteral("ui.theme"), QStringLiteral("dark")).ok());
    const Result<QString> value = settings_->getString(QStringLiteral("ui.theme"));
    QVERIFY(value);
    QCOMPARE(value.value(), QStringLiteral("dark"));
}

void SettingsStoreTest::storesAndReadsIntegers() {
    QVERIFY(settings_->setInteger(QStringLiteral("jobs.retry"), 5).ok());
    const Result<qint64> value = settings_->getInteger(QStringLiteral("jobs.retry"));
    QVERIFY(value);
    QCOMPARE(value.value(), qint64(5));
}

void SettingsStoreTest::storesAndReadsBooleans() {
    QVERIFY(settings_->setBoolean(QStringLiteral("safety.confirm"), false).ok());
    const Result<bool> value = settings_->getBoolean(QStringLiteral("safety.confirm"), true);
    QVERIFY(value);
    QVERIFY(!value.value());

    QVERIFY(settings_->setBoolean(QStringLiteral("safety.confirm"), true).ok());
    const Result<bool> updated = settings_->getBoolean(QStringLiteral("safety.confirm"), false);
    QVERIFY(updated);
    QVERIFY(updated.value());
}

void SettingsStoreTest::overwritesExistingKey() {
    QVERIFY(settings_->setString(QStringLiteral("ui.theme"), QStringLiteral("light")).ok());
    QVERIFY(settings_->setString(QStringLiteral("ui.theme"), QStringLiteral("system")).ok());
    const Result<QString> value = settings_->getString(QStringLiteral("ui.theme"));
    QVERIFY(value);
    QCOMPARE(value.value(), QStringLiteral("system"));
}

void SettingsStoreTest::persistsAcrossReopen() {
    const QString path = directory_->filePath(QStringLiteral("nesva.db"));
    QVERIFY(settings_->setString(QStringLiteral("persisted.key"), QStringLiteral("value-1")).ok());

    database_->close();
    QVERIFY(database_->open(path).ok());

    SettingsStore reopened(*database_);
    const Result<QString> value = reopened.getString(QStringLiteral("persisted.key"));
    QVERIFY(value);
    QCOMPARE(value.value(), QStringLiteral("value-1"));
}

void SettingsStoreTest::containsAndRemove() {
    QVERIFY(settings_->setString(QStringLiteral("temp.key"), QStringLiteral("x")).ok());

    const Result<bool> present = settings_->contains(QStringLiteral("temp.key"));
    QVERIFY(present);
    QVERIFY(present.value());

    QVERIFY(settings_->remove(QStringLiteral("temp.key")).ok());
    const Result<bool> gone = settings_->contains(QStringLiteral("temp.key"));
    QVERIFY(gone);
    QVERIFY(!gone.value());
}

void SettingsStoreTest::rejectsEmptyKey() {
    const Status status = settings_->setString(QStringLiteral("   "), QStringLiteral("x"));
    QVERIFY(!status.ok());
    QCOMPARE(static_cast<int>(status.error().code()), static_cast<int>(ErrorCode::InvalidArgument));
}

void SettingsStoreTest::listsAllSettings() {
    QVERIFY(settings_->setString(QStringLiteral("b.key"), QStringLiteral("2")).ok());
    QVERIFY(settings_->setString(QStringLiteral("a.key"), QStringLiteral("1")).ok());

    const Result<QMap<QString, QString>> all = settings_->all();
    QVERIFY(all);
    QCOMPARE(all.value().size(), 2);
    QCOMPARE(all.value().keys().first(), QStringLiteral("a.key"));
    QCOMPARE(all.value().value(QStringLiteral("a.key")), QStringLiteral("1"));
}

void SettingsStoreTest::handlesUnicodeValues() {
    const QString value = QStringLiteral("C:\\Users\\Jos\u00e9\\Dokumenty \u2014 predal");
    QVERIFY(settings_->setString(QStringLiteral("paths.unicode"), value).ok());
    const Result<QString> read = settings_->getString(QStringLiteral("paths.unicode"));
    QVERIFY(read);
    QCOMPARE(read.value(), value);
}

QTEST_GUILESS_MAIN(SettingsStoreTest)
#include "tst_settings_store.moc"
