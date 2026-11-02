# 0004. SQLite with forward-only migrations

- Status: Accepted
- Date: 2025-11-02

## Context

Nesva persists settings, job definitions, run history, manifests and (later) mailbox metadata
locally. The data model will evolve across six phases while users keep their databases.

## Decision

- SQLite through the Qt SQL `QSQLITE` driver; one connection per `Database` instance.
- Every connection sets: `PRAGMA foreign_keys = ON`, `busy_timeout = 5000`,
  `synchronous = NORMAL`, `journal_mode = WAL` (WAL is skipped for `:memory:` databases used by
  tests). The connection verifies that foreign keys are actually on and fails otherwise.
- Schema changes are **forward-only versioned migrations** (`db::Migration`): an ordered list of
  statements, each applied in its own transaction and recorded in `schema_migrations`.
  `migrate()` is idempotent and refuses to operate on a database newer than the binary.
- Prepared statements with positional bindings everywhere; no SQL string interpolation of user
  data.
- RAII `Transaction` with savepoint-based nesting so an inner failure cannot corrupt the outer
  unit of work; destruction without commit rolls back.
- Safety rules live in the schema where possible: `copy_semantics` constrained to `'copy'`,
  `allow_remote_deletions` defaulting to `0`, `CHECK` constraints on enumerations, `UNIQUE`
  constraints on identities, `ON DELETE CASCADE` for owned rows.
- Integrity is checkable at runtime (`PRAGMA integrity_check` + `PRAGMA foreign_key_check`),
  exposed through `nesva-cli doctor`.

## Consequences

- Upgrades are automatic and testable (`tst_database` covers idempotency and constraints).
- Downgrades are refused rather than risking corruption.
- Schema v1 already contains tables for later phases (snapshots, mail, restores) so the shape of
  the data model is reviewable now; later phases add columns/tables through new migrations.
- Queries must copy/move `QSqlQuery` values instead of copying them (Qt deprecation).

## Alternatives considered

- **QSettings/INI/JSON files**: no transactions, no relational integrity, no querying.
- **Embedding a document store (e.g. SQLite JSON1 only)**: loses referential integrity and
  indexed lookups that manifests need.
- **An ORM**: unnecessary complexity for a schema this small and would obscure SQL review.
