// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/db/database.hpp"

#include <QtCore/QDateTime>
#include <utility>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QUuid>
#include <QtSql/QSqlError>
#include <QtSql/QSqlRecord>

#include "nesva/core/logging.hpp"
#include "nesva/db/migrations.hpp"

namespace nesva::db {
namespace {

constexpr const char* kDriver = "QSQLITE";

std::string sqlErrorDetail(const QSqlError& error) {
    const QString driverText = error.driverText().trimmed();
    const QString databaseText = error.databaseText().trimmed();
    if (databaseText.isEmpty()) {
        return driverText.toStdString();
    }
    if (driverText.isEmpty()) {
        return databaseText.toStdString();
    }
    return (databaseText + ": " + driverText).toStdString();
}

} // namespace

Database::Database() : connectionName_(QStringLiteral("nesva-db-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))) {}

Database::~Database() {
    close();
}

Status Database::open(const QString& filePath) {
    if (open_) {
        close();
    }

    if (!QSqlDatabase::isDriverAvailable(QString::fromUtf8(kDriver))) {
        return makeError(ErrorCode::DatabaseError, "Qt SQLite driver (QSQLITE) is not available",
                         "Install the Qt SQL SQLite plugin for this Qt installation.");
    }

    filePath_ = QFileInfo(filePath).absoluteFilePath();

    if (filePath_ != QStringLiteral(":memory:")) {
        const QDir directory = QFileInfo(filePath_).absoluteDir();
        if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
            return makeError(ErrorCode::IoError, "cannot create database directory",
                             directory.absolutePath().toStdString());
        }
    }

    database_ = QSqlDatabase::addDatabase(QString::fromUtf8(kDriver), connectionName_);
    database_.setDatabaseName(filePath_);
    database_.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));

    if (!database_.open()) {
        const std::string detail = sqlErrorDetail(database_.lastError());
        close();
        return makeError(ErrorCode::DatabaseError, "cannot open database", detail);
    }
    open_ = true;

    Status pragmas = applyPragmas();
    if (!pragmas) {
        close();
        return pragmas;
    }

    logDebug("core.database", QStringLiteral("database opened"),
             QJsonObject{{QStringLiteral("path"), filePath_}});
    return Status();
}

Status Database::applyPragmas() {
    // Foreign key enforcement is off by default in SQLite and must be enabled
    // for every connection; schema integrity depends on it.
    QSqlQuery query(database_);
    const QStringList pragmas = {
        QStringLiteral("PRAGMA foreign_keys = ON"),
        QStringLiteral("PRAGMA busy_timeout = 5000"),
        QStringLiteral("PRAGMA synchronous = NORMAL"),
        QStringLiteral("PRAGMA temp_store = MEMORY"),
    };
    for (const QString& pragma : pragmas) {
        if (!query.exec(pragma)) {
            return makeError(ErrorCode::DatabaseError, "failed to apply PRAGMA",
                             QStringLiteral("%1: %2")
                                 .arg(pragma, query.lastError().text())
                                 .toStdString());
        }
    }

    // WAL is unsupported for in-memory databases; failure is not an error there.
    if (filePath_ != QStringLiteral(":memory:")) {
        if (!query.exec(QStringLiteral("PRAGMA journal_mode = WAL"))) {
            logWarn("core.database", QStringLiteral("could not enable WAL journal mode"),
                    QJsonObject{{QStringLiteral("error"), query.lastError().text()}});
        }
    }

    if (!query.exec(QStringLiteral("PRAGMA foreign_keys")) || !query.next() ||
        query.value(0).toInt() != 1) {
        return makeError(ErrorCode::DatabaseError,
                         "SQLite foreign key enforcement could not be enabled");
    }
    return Status();
}

void Database::close() {
    if (!open_) {
        return;
    }
    transactionDepth_ = 0;
    database_.close();
    database_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName_);
    open_ = false;
}

bool Database::isOpen() const {
    return open_ && database_.isOpen();
}

Status Database::migrate() {
    if (!isOpen()) {
        return makeError(ErrorCode::DatabaseError, "cannot migrate a closed database");
    }

    Status bootstrap = ensureMigrationTable();
    if (!bootstrap) {
        return bootstrap;
    }

    Result<int> applied = schemaVersion();
    if (!applied) {
        return applied.error();
    }

    const int currentVersion = applied.value();
    if (currentVersion > latestSchemaVersion()) {
        return makeError(
            ErrorCode::DatabaseError, "database schema is newer than this application build",
            QStringLiteral("database=%1 application=%2")
                .arg(currentVersion)
                .arg(latestSchemaVersion())
                .toStdString());
    }

    for (const Migration& migration : allMigrations()) {
        if (migration.version <= currentVersion) {
            continue;
        }

        Transaction transaction(*this);
        QSqlQuery statement(database_);
        for (const QString& sql : migration.statements) {
            if (!statement.exec(sql)) {
                const std::string detail = QStringLiteral("migration %1 (%2) statement failed: %3")
                                               .arg(migration.version)
                                               .arg(migration.name, statement.lastError().text())
                                               .toStdString();
                return makeError(ErrorCode::DatabaseError, "schema migration failed", detail);
            }
        }

        QSqlQuery record(database_);
        record.prepare(QStringLiteral(
            "INSERT INTO schema_migrations (version, name, applied_at) VALUES (?, ?, ?)"));
        record.addBindValue(migration.version);
        record.addBindValue(migration.name);
        record.addBindValue(currentTimestampUtc());
        if (!record.exec()) {
            return makeError(ErrorCode::DatabaseError, "cannot record applied migration",
                             record.lastError().text().toStdString());
        }

        Status committed = transaction.commit();
        if (!committed) {
            return committed;
        }

        logInfo("core.database", QStringLiteral("schema migration applied"),
                QJsonObject{{QStringLiteral("version"), migration.version},
                            {QStringLiteral("name"), migration.name}});
    }

    return Status();
}

Status Database::ensureMigrationTable() {
    for (const QString& statement : bootstrapStatements()) {
        QSqlQuery query(database_);
        if (!query.exec(statement)) {
            return makeError(ErrorCode::DatabaseError,
                             "cannot create migration bookkeeping table",
                             query.lastError().text().toStdString());
        }
    }
    return Status();
}

Result<int> Database::schemaVersion() const {
    if (!isOpen()) {
        return makeErrorValue(ErrorCode::DatabaseError, "cannot read schema version from a closed database");
    }

    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_migrations"))) {
        // The bookkeeping table does not exist yet on a pristine database.
        if (query.lastError().text().contains(QStringLiteral("no such table"))) {
            return 0;
        }
        return makeErrorValue(ErrorCode::DatabaseError, "cannot read schema version",
                              query.lastError().text().toStdString());
    }
    if (!query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

int Database::latestSchemaVersion() {
    return db::latestSchemaVersion();
}

Status Database::exec(const QString& statement) {
    if (!isOpen()) {
        return makeError(ErrorCode::DatabaseError, "cannot execute statement on a closed database");
    }
    QSqlQuery query(database_);
    if (!query.exec(statement)) {
        return makeError(ErrorCode::DatabaseError, "statement failed",
                         sqlErrorDetail(query.lastError()));
    }
    return Status();
}

Result<QSqlQuery> Database::prepare(const QString& statement) const {
    if (!isOpen()) {
        return makeErrorValue(ErrorCode::DatabaseError, "cannot prepare statement on a closed database");
    }
    QSqlQuery query(database_);
    if (!query.prepare(statement)) {
        return makeErrorValue(ErrorCode::DatabaseError, "cannot prepare statement",
                              sqlErrorDetail(query.lastError()));
    }
    return query;
}

Result<QSqlQuery> Database::select(const QString& statement, const QVariantList& bindings) const {
    Result<QSqlQuery> prepared = prepare(statement);
    if (!prepared) {
        return prepared.error();
    }
    QSqlQuery query = std::move(prepared).take();
    for (const QVariant& binding : bindings) {
        query.addBindValue(binding);
    }
    if (!query.exec()) {
        return makeErrorValue(ErrorCode::DatabaseError, "query failed",
                              sqlErrorDetail(query.lastError()));
    }
    return query;
}

Status Database::beginTransaction() {
    if (!isOpen()) {
        return makeError(ErrorCode::DatabaseError, "cannot begin transaction on a closed database");
    }

    QSqlQuery query(database_);
    if (transactionDepth_ == 0) {
        if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
            return makeError(ErrorCode::DatabaseError, "cannot begin transaction",
                             sqlErrorDetail(query.lastError()));
        }
    } else {
        // Nested transactions use savepoints so that inner rollbacks do not
        // abort the outer unit of work.
        const QString savepoint = QStringLiteral("nesva_savepoint_%1").arg(transactionDepth_);
        if (!query.exec(QStringLiteral("SAVEPOINT %1").arg(savepoint))) {
            return makeError(ErrorCode::DatabaseError, "cannot create savepoint",
                             sqlErrorDetail(query.lastError()));
        }
    }
    ++transactionDepth_;
    return Status();
}

Status Database::commit() {
    if (transactionDepth_ <= 0) {
        return makeError(ErrorCode::DatabaseError, "commit without an active transaction");
    }

    QSqlQuery query(database_);
    --transactionDepth_;
    if (transactionDepth_ == 0) {
        if (!query.exec(QStringLiteral("COMMIT"))) {
            transactionDepth_ = 1;
            return makeError(ErrorCode::DatabaseError, "cannot commit transaction",
                             sqlErrorDetail(query.lastError()));
        }
    } else {
        const QString savepoint = QStringLiteral("nesva_savepoint_%1").arg(transactionDepth_);
        if (!query.exec(QStringLiteral("RELEASE %1").arg(savepoint))) {
            ++transactionDepth_;
            return makeError(ErrorCode::DatabaseError, "cannot release savepoint",
                             sqlErrorDetail(query.lastError()));
        }
    }
    return Status();
}

Status Database::rollback() {
    if (transactionDepth_ <= 0) {
        return makeError(ErrorCode::DatabaseError, "rollback without an active transaction");
    }

    QSqlQuery query(database_);
    --transactionDepth_;
    const QString statement =
        transactionDepth_ == 0
            ? QStringLiteral("ROLLBACK")
            : QStringLiteral("ROLLBACK TO %1").arg(
                  QStringLiteral("nesva_savepoint_%1").arg(transactionDepth_));
    if (!query.exec(statement)) {
        ++transactionDepth_;
        return makeError(ErrorCode::DatabaseError, "cannot roll back transaction",
                         sqlErrorDetail(query.lastError()));
    }
    return Status();
}

Status Database::checkIntegrity(QString* details) const {
    if (!isOpen()) {
        return makeError(ErrorCode::DatabaseError, "cannot check a closed database");
    }

    QSqlQuery integrity(database_);
    if (!integrity.exec(QStringLiteral("PRAGMA integrity_check"))) {
        return makeError(ErrorCode::DatabaseError, "integrity check failed to run",
                         sqlErrorDetail(integrity.lastError()));
    }

    QStringList lines;
    bool ok = true;
    while (integrity.next()) {
        const QString line = integrity.value(0).toString();
        lines.append(line);
        if (line.compare(QStringLiteral("ok"), Qt::CaseInsensitive) != 0) {
            ok = false;
        }
    }

    QSqlQuery foreignKeys(database_);
    if (!foreignKeys.exec(QStringLiteral("PRAGMA foreign_key_check"))) {
        return makeError(ErrorCode::DatabaseError, "foreign key check failed to run",
                         sqlErrorDetail(foreignKeys.lastError()));
    }
    int violations = 0;
    while (foreignKeys.next()) {
        ++violations;
        lines.append(QStringLiteral("foreign key violation: table %1 rowid %2")
                         .arg(foreignKeys.value(0).toString(),
                              foreignKeys.value(1).toString()));
    }
    if (violations > 0) {
        ok = false;
    }

    if (details != nullptr) {
        *details = lines.join(QLatin1Char('\n'));
    }
    if (!ok) {
        return makeError(ErrorCode::IntegrityError, "database integrity check reported problems");
    }
    return Status();
}

QString Database::currentTimestampUtc() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

Transaction::Transaction(Database& database) : database_(&database) {
    if (database_->beginTransaction().ok()) {
        active_ = true;
    }
}

Transaction::~Transaction() {
    if (active_) {
        // Destruction without commit() rolls back: never leave a transaction
        // open on the connection.
        database_->rollback();
        active_ = false;
    }
}

Status Transaction::commit() {
    if (!active_) {
        return makeError(ErrorCode::Internal, "transaction is not active");
    }
    Status status = database_->commit();
    if (status) {
        active_ = false;
    }
    return status;
}

Status Transaction::rollback() {
    if (!active_) {
        return makeError(ErrorCode::Internal, "transaction is not active");
    }
    Status status = database_->rollback();
    active_ = false;
    return status;
}

} // namespace nesva::db
