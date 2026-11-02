// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include <utility>

#include "nesva/db/database.hpp"
#include "nesva/db/migrations.hpp"

using namespace nesva;
using nesva::db::Database;
using nesva::db::Transaction;

class DatabaseTest : public QObject {
    Q_OBJECT

private slots:
    void opensAndCreatesFile();
    void migrateCreatesSchema();
    void migrateIsIdempotent();
    void foreignKeysAreEnforced();
    void walJournalModeIsEnabled();
    void transactionRollbackDiscardsChanges();
    void transactionRaiiRollsBackWithoutCommit();
    void nestedTransactionUsesSavepoints();
    void preparedStatementsBindValues();
    void uniqueConstraintViolationIsReported();
    void invalidStatementReportsError();
    void integrityCheckSucceeds();
    void timestampsAreUtcIso();
    void migrationCatalogIsConsistent();
};

namespace {

int countRows(Database& database, const QString& table) {
    Result<QSqlQuery> query =
        database.select(QStringLiteral("SELECT COUNT(*) FROM %1").arg(table));
    if (!query) {
        return -1;
    }
    QSqlQuery rows = std::move(query).take();
    return rows.next() ? rows.value(0).toInt() : -1;
}

} // namespace

void DatabaseTest::opensAndCreatesFile() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("db/nesva.db"));

    Database database;
    QVERIFY(database.open(path).ok());
    QVERIFY(database.isOpen());
    QVERIFY(QFileInfo::exists(path));
    database.close();
    QVERIFY(!database.isOpen());
}

void DatabaseTest::migrateCreatesSchema() {
    QTemporaryDir directory;
    Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("nesva.db"))).ok());

    const Result<int> before = database.schemaVersion();
    QVERIFY(before);
    QCOMPARE(before.value(), 0);

    QVERIFY(database.migrate().ok());

    const Result<int> after = database.schemaVersion();
    QVERIFY(after);
    QCOMPARE(after.value(), db::latestSchemaVersion());

    // Every table promised by the architecture exists after migration.
    const QStringList expected = {
        QStringLiteral("settings"),          QStringLiteral("storage_providers"),
        QStringLiteral("storage_credentials_refs"), QStringLiteral("backup_jobs"),
        QStringLiteral("backup_sources"),    QStringLiteral("backup_exclusions"),
        QStringLiteral("job_schedules"),     QStringLiteral("job_runs"),
        QStringLiteral("job_events"),        QStringLiteral("file_manifest"),
        QStringLiteral("archive_objects"),   QStringLiteral("backup_snapshots"),
        QStringLiteral("restore_jobs"),      QStringLiteral("mail_accounts"),
        QStringLiteral("mail_folders"),      QStringLiteral("mail_messages"),
        QStringLiteral("application_events"),
    };
    for (const QString& table : expected) {
        Result<QSqlQuery> query = database.select(
            QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' AND name = ?"),
            {QVariant(table)});
        QVERIFY2(query, qPrintable(QStringLiteral("query failed for %1").arg(table)));
        QSqlQuery rows = std::move(query).take();
        QVERIFY2(rows.next(), qPrintable(QStringLiteral("missing table: %1").arg(table)));
    }
}

void DatabaseTest::migrateIsIdempotent() {
    QTemporaryDir directory;
    Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("nesva.db"))).ok());
    QVERIFY(database.migrate().ok());
    QVERIFY(database.migrate().ok());

    Result<QSqlQuery> query =
        database.select(QStringLiteral("SELECT COUNT(*) FROM schema_migrations"));
    QVERIFY(query);
    QSqlQuery rows = std::move(query).take();
    QVERIFY(rows.next());
    QCOMPARE(rows.value(0).toInt(), db::latestSchemaVersion());
}

void DatabaseTest::foreignKeysAreEnforced() {
    QTemporaryDir directory;
    Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("nesva.db"))).ok());
    QVERIFY(database.migrate().ok());

    Result<QSqlQuery> pragma = database.select(QStringLiteral("PRAGMA foreign_keys"));
    QVERIFY(pragma);
    QSqlQuery pragmaRows = std::move(pragma).take();
    QVERIFY(pragmaRows.next());
    QCOMPARE(pragmaRows.value(0).toInt(), 1);

    // A source row referencing a non-existent job must be rejected.
    const Status inserted = database.exec(QStringLiteral(
        "INSERT INTO backup_sources (job_id, path, created_at) "
        "VALUES (9999, '/tmp/x', '2025-01-01T00:00:00.000Z')"));
    QVERIFY(!inserted.ok());
    QCOMPARE(static_cast<int>(inserted.error().code()), static_cast<int>(ErrorCode::DatabaseError));
}

void DatabaseTest::walJournalModeIsEnabled() {
    QTemporaryDir directory;
    Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("nesva.db"))).ok());

    Result<QSqlQuery> query = database.select(QStringLiteral("PRAGMA journal_mode"));
    QVERIFY(query);
    QSqlQuery rows = std::move(query).take();
    QVERIFY(rows.next());
    QCOMPARE(rows.value(0).toString().toLower(), QStringLiteral("wal"));
}

void DatabaseTest::transactionRollbackDiscardsChanges() {
    QTemporaryDir directory;
    Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("nesva.db"))).ok());
    QVERIFY(database.migrate().ok());

    QVERIFY(database.beginTransaction().ok());
    QVERIFY(database.exec(QStringLiteral(
                               "INSERT INTO settings (key, value, value_type, updated_at) "
                               "VALUES ('unit.rollback', 'x', 'string', '2025-01-01T00:00:00.000Z')"))
                .ok());
    QCOMPARE(countRows(database, QStringLiteral("settings")), 1);
    QVERIFY(database.rollback().ok());
    QCOMPARE(countRows(database, QStringLiteral("settings")), 0);
}

void DatabaseTest::transactionRaiiRollsBackWithoutCommit() {
    QTemporaryDir directory;
    Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("nesva.db"))).ok());
    QVERIFY(database.migrate().ok());

    {
        Transaction transaction(database);
        QVERIFY(transaction.isActive());
        QVERIFY(database.exec(QStringLiteral(
                                  "INSERT INTO settings (key, value, value_type, updated_at) "
                                  "VALUES ('unit.raii', 'x', 'string', '2025-01-01T00:00:00.000Z')"))
                    .ok());
        // No commit: the destructor must roll back.
    }
    QCOMPARE(countRows(database, QStringLiteral("settings")), 0);
    QVERIFY(!database.inTransaction());

    {
        Transaction transaction(database);
        QVERIFY(database.exec(QStringLiteral(
                                  "INSERT INTO settings (key, value, value_type, updated_at) "
                                  "VALUES ('unit.raii', 'x', 'string', '2025-01-01T00:00:00.000Z')"))
                    .ok());
        QVERIFY(transaction.commit().ok());
    }
    QCOMPARE(countRows(database, QStringLiteral("settings")), 1);
}

void DatabaseTest::nestedTransactionUsesSavepoints() {
    QTemporaryDir directory;
    Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("nesva.db"))).ok());
    QVERIFY(database.migrate().ok());

    Transaction outer(database);
    QVERIFY(database.exec(QStringLiteral(
                              "INSERT INTO settings (key, value, value_type, updated_at) "
                              "VALUES ('unit.outer', 'x', 'string', '2025-01-01T00:00:00.000Z')"))
                .ok());

    {
        Transaction inner(database);
        QVERIFY(database.exec(QStringLiteral(
                                  "INSERT INTO settings (key, value, value_type, updated_at) "
                                  "VALUES ('unit.inner', 'x', 'string', '2025-01-01T00:00:00.000Z')"))
                    .ok());
        QVERIFY(inner.rollback().ok());
    }

    QVERIFY(outer.commit().ok());
    QCOMPARE(countRows(database, QStringLiteral("settings")), 1);
}

void DatabaseTest::preparedStatementsBindValues() {
    QTemporaryDir directory;
    Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("nesva.db"))).ok());
    QVERIFY(database.migrate().ok());

    Result<QSqlQuery> statement = database.prepare(QStringLiteral(
        "INSERT INTO settings (key, value, value_type, updated_at) VALUES (?, ?, ?, ?)"));
    QVERIFY(statement);
    QSqlQuery insert = std::move(statement).take();
    insert.addBindValue(QStringLiteral("unit.bound"));
    insert.addBindValue(QStringLiteral("v\u00e4lue with unicode \u00e6\u00f8\u00e5 and spaces"));
    insert.addBindValue(QStringLiteral("string"));
    insert.addBindValue(Database::currentTimestampUtc());
    QVERIFY(insert.exec());

    Result<QSqlQuery> query = database.select(
        QStringLiteral("SELECT value FROM settings WHERE key = ?"),
        {QVariant(QStringLiteral("unit.bound"))});
    QVERIFY(query);
    QSqlQuery rows = std::move(query).take();
    QVERIFY(rows.next());
    QCOMPARE(rows.value(0).toString(),
             QStringLiteral("v\u00e4lue with unicode \u00e6\u00f8\u00e5 and spaces"));
}

void DatabaseTest::uniqueConstraintViolationIsReported() {
    QTemporaryDir directory;
    Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("nesva.db"))).ok());
    QVERIFY(database.migrate().ok());

    const QString insert = QStringLiteral(
        "INSERT INTO backup_jobs (name, mode, created_at, updated_at) "
        "VALUES ('Nightly', 'archive', '2025-01-01T00:00:00.000Z', '2025-01-01T00:00:00.000Z')");
    QVERIFY(database.exec(insert).ok());
    const Status duplicate = database.exec(insert);
    QVERIFY(!duplicate.ok());
    QCOMPARE(static_cast<int>(duplicate.error().code()), static_cast<int>(ErrorCode::DatabaseError));
}

void DatabaseTest::invalidStatementReportsError() {
    QTemporaryDir directory;
    Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("nesva.db"))).ok());

    const Status status = database.exec(QStringLiteral("SELECT * FROM does_not_exist"));
    QVERIFY(!status.ok());
    QVERIFY(status.error().message().find("statement failed") != std::string::npos);

    const Result<QSqlQuery> prepared = database.prepare(QStringLiteral("INSERT INTO nonsense VALUES"));
    QVERIFY(!prepared);
}

void DatabaseTest::integrityCheckSucceeds() {
    QTemporaryDir directory;
    Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("nesva.db"))).ok());
    QVERIFY(database.migrate().ok());

    QString details;
    QVERIFY(database.checkIntegrity(&details).ok());
    QVERIFY(details.contains(QStringLiteral("ok")));
}

void DatabaseTest::timestampsAreUtcIso() {
    const QString timestamp = Database::currentTimestampUtc();
    QVERIFY(timestamp.endsWith(QLatin1Char('Z')));
    QVERIFY(timestamp.contains(QLatin1Char('T')));
    const QDateTime parsed = QDateTime::fromString(timestamp, Qt::ISODateWithMs);
    QVERIFY(parsed.isValid());
}

void DatabaseTest::migrationCatalogIsConsistent() {
    const auto& migrations = db::allMigrations();
    QVERIFY(!migrations.empty());
    int previous = 0;
    for (const db::Migration& migration : migrations) {
        QVERIFY2(migration.version > previous, "migration versions must increase");
        QVERIFY(!migration.name.isEmpty());
        QVERIFY(!migration.statements.isEmpty());
        QCOMPARE(db::findMigration(migration.version)->name, migration.name);
        previous = migration.version;
    }
    QCOMPARE(db::latestSchemaVersion(), previous);
    QVERIFY(db::findMigration(previous + 100) == nullptr);
}

QTEST_GUILESS_MAIN(DatabaseTest)
#include "tst_database.moc"
