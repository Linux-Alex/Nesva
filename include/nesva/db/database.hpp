// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>
#include <QtCore/QVariantList>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>

#include "nesva/core/result.hpp"

namespace nesva::db {

class Transaction;

/// Owns one SQLite connection to the Nesva database.
///
/// Thread affinity: a Database instance, and every QSqlQuery it hands out,
/// must be used from a single thread at a time. Create one instance per thread.
///
/// Every connection is opened with foreign key enforcement, WAL journaling,
/// a busy timeout, and normal synchronous mode.
class Database {
public:
    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /// Opens (and creates when missing) the SQLite database file.
    Status open(const QString& filePath);
    void close();
    bool isOpen() const;

    QString filePath() const { return filePath_; }
    QString connectionName() const { return connectionName_; }
    QSqlDatabase& handle() { return database_; }

    /// Applies all pending migrations inside a transaction per migration.
    /// Idempotent: running it on an up-to-date database is a no-op.
    Status migrate();

    /// Highest applied migration version, or 0 for a fresh database.
    Result<int> schemaVersion() const;
    static int latestSchemaVersion();

    /// Executes a statement (or a batch of statements separated by ';' at the
    /// end of a line) without returning rows.
    Status exec(const QString& statement);

    /// Returns a prepared but unexecuted query. Bind values with
    /// QSqlQuery::bindValue and call exec().
    Result<QSqlQuery> prepare(const QString& statement) const;

    /// Returns an executed query with the supplied positional bindings.
    Result<QSqlQuery> select(const QString& statement, const QVariantList& bindings = {}) const;

    Status beginTransaction();
    Status commit();
    Status rollback();
    bool inTransaction() const { return transactionDepth_ > 0; }

    /// Runs PRAGMA integrity_check and PRAGMA foreign_key_check.
    /// On success \p details receives the raw integrity_check output.
    Status checkIntegrity(QString* details) const;

    /// Convenience UTC timestamp in the ISO-8601 format used by every table.
    static QString currentTimestampUtc();

private:
    friend class Transaction;

    Status applyPragmas();
    Status ensureMigrationTable();

    QSqlDatabase database_;
    QString connectionName_;
    QString filePath_;
    bool open_ = false;
    int transactionDepth_ = 0;
};

/// RAII transaction. Rolls back on destruction unless commit() succeeded.
class Transaction {
public:
    explicit Transaction(Database& database);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    Status commit();
    Status rollback();
    bool isActive() const { return active_; }

private:
    Database* database_ = nullptr;
    bool active_ = false;
};

} // namespace nesva::db
