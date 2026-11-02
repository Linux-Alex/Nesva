// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <vector>

namespace nesva::db {

/// A single forward-only schema migration.
///
/// Statements are executed in order inside one transaction. SQLite's Qt driver
/// executes a single statement per call, so migrations are expressed as a list
/// of statements rather than one script.
struct Migration {
    int version = 0;
    QString name;
    QString description;
    QStringList statements;
};

/// All known migrations, ordered by ascending version.
const std::vector<Migration>& allMigrations();

/// Highest schema version understood by this build.
int latestSchemaVersion();

/// Returns the migration with the given version, or nullptr.
const Migration* findMigration(int version);

/// Bootstrap DDL for the migration bookkeeping table itself. Runs before any
/// versioned migration and is idempotent.
const QStringList& bootstrapStatements();

} // namespace nesva::db
