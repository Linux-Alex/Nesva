// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/db/migrations.hpp"

namespace nesva::db {
namespace {

QStringList migrationV1Statements() {
    return {
        // ---------------------------------------------------------------- settings
        QStringLiteral(R"SQL(
CREATE TABLE settings (
    key         TEXT PRIMARY KEY NOT NULL,
    value       TEXT,
    value_type  TEXT NOT NULL DEFAULT 'string'
                CHECK (value_type IN ('string','integer','real','boolean','json')),
    updated_at  TEXT NOT NULL
))SQL"),

        // --------------------------------------------------------------- storage
        QStringLiteral(R"SQL(
CREATE TABLE storage_providers (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    name                TEXT NOT NULL UNIQUE,
    provider_type       TEXT NOT NULL
                        CHECK (provider_type IN ('local','rclone','s3','sftp','webdav','icloud','other')),
    rclone_remote       TEXT,
    base_path           TEXT,
    configuration_json  TEXT NOT NULL DEFAULT '{}',
    capabilities_json   TEXT NOT NULL DEFAULT '{}',
    status              TEXT NOT NULL DEFAULT 'unknown'
                        CHECK (status IN ('unknown','ok','error','auth_required')),
    status_detail       TEXT,
    last_checked_at     TEXT,
    created_at          TEXT NOT NULL,
    updated_at          TEXT NOT NULL
))SQL"),

        QStringLiteral(R"SQL(
CREATE TABLE storage_credentials_refs (
    id                   INTEGER PRIMARY KEY AUTOINCREMENT,
    storage_provider_id  INTEGER NOT NULL
                         REFERENCES storage_providers(id) ON DELETE CASCADE,
    secret_kind          TEXT NOT NULL
                         CHECK (secret_kind IN ('password','token','oauth_refresh',
                                                'restic_repository_password','other')),
    keyring_backend      TEXT NOT NULL
                         CHECK (keyring_backend IN ('windows_credential_manager',
                                                    'linux_secret_service',
                                                    'macos_keychain','none')),
    secret_reference     TEXT NOT NULL,
    created_at           TEXT NOT NULL,
    updated_at           TEXT NOT NULL,
    UNIQUE (storage_provider_id, secret_kind)
))SQL"),

        // ------------------------------------------------------------------ jobs
        QStringLiteral(R"SQL(
CREATE TABLE backup_jobs (
    id                        INTEGER PRIMARY KEY AUTOINCREMENT,
    name                      TEXT NOT NULL UNIQUE,
    mode                      TEXT NOT NULL CHECK (mode IN ('archive','backup')),
    storage_provider_id       INTEGER
                              REFERENCES storage_providers(id) ON DELETE SET NULL,
    destination_path          TEXT,
    enabled                   INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0,1)),
    bandwidth_limit_kib       INTEGER
                              CHECK (bandwidth_limit_kib IS NULL OR bandwidth_limit_kib > 0),
    retention_policy_json     TEXT NOT NULL DEFAULT '{}',
    verification_options_json TEXT NOT NULL DEFAULT '{}',
    notification_options_json TEXT NOT NULL DEFAULT '{}',
    copy_semantics             TEXT NOT NULL DEFAULT 'copy'
                              CHECK (copy_semantics IN ('copy')),
    allow_remote_deletions    INTEGER NOT NULL DEFAULT 0
                              CHECK (allow_remote_deletions IN (0,1)),
    created_at                TEXT NOT NULL,
    updated_at                TEXT NOT NULL
))SQL"),

        QStringLiteral(R"SQL(
CREATE TABLE backup_sources (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id      INTEGER NOT NULL REFERENCES backup_jobs(id) ON DELETE CASCADE,
    path        TEXT NOT NULL,
    is_primary  INTEGER NOT NULL DEFAULT 1 CHECK (is_primary IN (0,1)),
    created_at  TEXT NOT NULL,
    UNIQUE (job_id, path)
))SQL"),

        QStringLiteral(R"SQL(
CREATE TABLE backup_exclusions (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id      INTEGER NOT NULL REFERENCES backup_jobs(id) ON DELETE CASCADE,
    pattern     TEXT NOT NULL,
    match_kind  TEXT NOT NULL DEFAULT 'glob'
                CHECK (match_kind IN ('glob','regex','literal')),
    created_at  TEXT NOT NULL,
    UNIQUE (job_id, pattern)
))SQL"),

        QStringLiteral(R"SQL(
CREATE TABLE job_schedules (
    id                    INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id                INTEGER NOT NULL UNIQUE
                          REFERENCES backup_jobs(id) ON DELETE CASCADE,
    trigger_kind          TEXT NOT NULL
                          CHECK (trigger_kind IN ('manual','daily','weekly','monthly',
                                                  'on_login','on_startup')),
    time_of_day           TEXT CHECK (time_of_day IS NULL OR
                                      time_of_day GLOB '[0-2][0-9]:[0-5][0-9]'),
    day_of_week           INTEGER CHECK (day_of_week IS NULL OR
                                         (day_of_week BETWEEN 0 AND 6)),
    day_of_month          INTEGER CHECK (day_of_month IS NULL OR
                                         (day_of_month BETWEEN 1 AND 28)),
    timezone              TEXT NOT NULL DEFAULT 'UTC',
    max_duration_seconds  INTEGER,
    retry_max_attempts    INTEGER NOT NULL DEFAULT 0,
    retry_backoff_seconds INTEGER NOT NULL DEFAULT 60,
    catch_up_missed       INTEGER NOT NULL DEFAULT 1 CHECK (catch_up_missed IN (0,1)),
    next_run_at           TEXT,
    last_run_at           TEXT,
    created_at            TEXT NOT NULL,
    updated_at            TEXT NOT NULL
))SQL"),

        QStringLiteral(R"SQL(
CREATE TABLE job_runs (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id             INTEGER NOT NULL REFERENCES backup_jobs(id) ON DELETE CASCADE,
    run_number         INTEGER NOT NULL,
    trigger_kind       TEXT NOT NULL,
    status             TEXT NOT NULL
                       CHECK (status IN ('queued','running','succeeded','failed',
                                         'cancelled','interrupted')),
    started_at         TEXT,
    finished_at        TEXT,
    files_total        INTEGER NOT NULL DEFAULT 0,
    files_transferred  INTEGER NOT NULL DEFAULT 0,
    files_skipped      INTEGER NOT NULL DEFAULT 0,
    files_failed       INTEGER NOT NULL DEFAULT 0,
    bytes_total        INTEGER NOT NULL DEFAULT 0,
    bytes_transferred  INTEGER NOT NULL DEFAULT 0,
    error_code         TEXT,
    error_message      TEXT,
    created_at         TEXT NOT NULL,
    UNIQUE (job_id, run_number)
))SQL"),

        QStringLiteral(
            "CREATE INDEX idx_job_runs_job_started ON job_runs(job_id, started_at DESC)"),
        QStringLiteral("CREATE INDEX idx_job_runs_status ON job_runs(status)"),

        QStringLiteral(R"SQL(
CREATE TABLE job_events (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id      INTEGER REFERENCES backup_jobs(id) ON DELETE CASCADE,
    job_run_id  INTEGER REFERENCES job_runs(id) ON DELETE CASCADE,
    level       TEXT NOT NULL
                CHECK (level IN ('trace','debug','info','warn','error','critical')),
    category    TEXT NOT NULL,
    message     TEXT NOT NULL,
    data_json   TEXT,
    occurred_at TEXT NOT NULL
))SQL"),

        QStringLiteral("CREATE INDEX idx_job_events_run ON job_events(job_run_id, id)"),
        QStringLiteral(
            "CREATE INDEX idx_job_events_occurred ON job_events(occurred_at DESC)"),

        // -------------------------------------------------------------- manifests
        QStringLiteral(R"SQL(
CREATE TABLE file_manifest (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id         INTEGER NOT NULL REFERENCES backup_jobs(id) ON DELETE CASCADE,
    job_run_id     INTEGER NOT NULL REFERENCES job_runs(id) ON DELETE CASCADE,
    relative_path  TEXT NOT NULL,
    source_path    TEXT NOT NULL,
    remote_path    TEXT,
    size_bytes     INTEGER NOT NULL DEFAULT 0,
    mtime_utc      TEXT,
    sha256         TEXT,
    remote_hash    TEXT,
    state          TEXT NOT NULL DEFAULT 'pending'
                   CHECK (state IN ('pending','transferred','skipped','failed','verified')),
    created_at     TEXT NOT NULL,
    updated_at     TEXT NOT NULL,
    UNIQUE (job_run_id, relative_path)
))SQL"),

        QStringLiteral(
            "CREATE INDEX idx_file_manifest_job ON file_manifest(job_id, relative_path)"),

        QStringLiteral(R"SQL(
CREATE TABLE archive_objects (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id       INTEGER NOT NULL REFERENCES backup_jobs(id) ON DELETE CASCADE,
    job_run_id   INTEGER REFERENCES job_runs(id) ON DELETE SET NULL,
    remote_path  TEXT NOT NULL,
    size_bytes   INTEGER,
    sha256       TEXT,
    remote_hash  TEXT,
    uploaded_at  TEXT,
    verified_at  TEXT,
    state        TEXT NOT NULL DEFAULT 'present'
                 CHECK (state IN ('present','missing','modified','unverified')),
    UNIQUE (job_id, remote_path)
))SQL"),

        // -------------------------------------------------------------- snapshots
        QStringLiteral(R"SQL(
CREATE TABLE backup_snapshots (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id          INTEGER NOT NULL REFERENCES backup_jobs(id) ON DELETE CASCADE,
    snapshot_id     TEXT NOT NULL,
    hostname        TEXT,
    repository_path TEXT,
    created_at      TEXT,
    tags_json       TEXT NOT NULL DEFAULT '[]',
    paths_json      TEXT NOT NULL DEFAULT '[]',
    size_bytes      INTEGER,
    verified_at     TEXT,
    UNIQUE (job_id, snapshot_id)
))SQL"),

        QStringLiteral(R"SQL(
CREATE TABLE restore_jobs (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id          INTEGER NOT NULL REFERENCES backup_jobs(id) ON DELETE CASCADE,
    source_kind     TEXT NOT NULL CHECK (source_kind IN ('archive','snapshot')),
    source_selector TEXT NOT NULL,
    destination_path TEXT NOT NULL,
    conflict_policy TEXT NOT NULL DEFAULT 'ask'
                    CHECK (conflict_policy IN ('ask','skip','overwrite','rename')),
    status          TEXT NOT NULL DEFAULT 'queued'
                    CHECK (status IN ('queued','running','succeeded','failed','cancelled')),
    started_at      TEXT,
    finished_at     TEXT,
    files_restored  INTEGER NOT NULL DEFAULT 0,
    bytes_restored  INTEGER NOT NULL DEFAULT 0,
    error_message   TEXT,
    created_at      TEXT NOT NULL
))SQL"),

        // ------------------------------------------------------------------ mail
        QStringLiteral(R"SQL(
CREATE TABLE mail_accounts (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    display_name       TEXT NOT NULL,
    tenant_id          TEXT NOT NULL,
    client_id          TEXT NOT NULL,
    account_identifier TEXT NOT NULL UNIQUE,
    credential_ref_id  INTEGER
                       REFERENCES storage_credentials_refs(id) ON DELETE SET NULL,
    scopes             TEXT NOT NULL DEFAULT '',
    status             TEXT NOT NULL DEFAULT 'disconnected'
                       CHECK (status IN ('disconnected','connected','auth_required','error')),
    last_sync_at       TEXT,
    created_at         TEXT NOT NULL,
    updated_at         TEXT NOT NULL
))SQL"),

        QStringLiteral(R"SQL(
CREATE TABLE mail_folders (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    mail_account_id         INTEGER NOT NULL REFERENCES mail_accounts(id) ON DELETE CASCADE,
    remote_folder_id        TEXT NOT NULL,
    display_name            TEXT NOT NULL,
    parent_remote_folder_id TEXT,
    delta_link              TEXT,
    sync_enabled            INTEGER NOT NULL DEFAULT 1 CHECK (sync_enabled IN (0,1)),
    last_sync_at            TEXT,
    created_at              TEXT NOT NULL,
    UNIQUE (mail_account_id, remote_folder_id)
))SQL"),

        QStringLiteral(R"SQL(
CREATE TABLE mail_messages (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    mail_account_id     INTEGER NOT NULL REFERENCES mail_accounts(id) ON DELETE CASCADE,
    mail_folder_id      INTEGER REFERENCES mail_folders(id) ON DELETE SET NULL,
    remote_message_id   TEXT NOT NULL,
    internet_message_id TEXT,
    subject             TEXT,
    sender              TEXT,
    received_at         TEXT,
    message_key         TEXT NOT NULL,
    eml_path            TEXT,
    eml_sha256          TEXT,
    export_state        TEXT NOT NULL DEFAULT 'pending'
                        CHECK (export_state IN ('pending','exported','failed','skipped')),
    last_modified_at    TEXT,
    created_at          TEXT NOT NULL,
    updated_at          TEXT NOT NULL,
    UNIQUE (mail_account_id, remote_message_id)
))SQL"),

        QStringLiteral(
            "CREATE INDEX idx_mail_messages_key ON mail_messages(mail_account_id, message_key)"),

        // --------------------------------------------------------------- auditing
        QStringLiteral(R"SQL(
CREATE TABLE application_events (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    level      TEXT NOT NULL
               CHECK (level IN ('trace','debug','info','warn','error','critical')),
    category   TEXT NOT NULL,
    message    TEXT NOT NULL,
    data_json  TEXT,
    created_at TEXT NOT NULL
))SQL"),

        QStringLiteral(
            "CREATE INDEX idx_application_events_created ON application_events(created_at DESC)"),
    };
}

QStringList migrationV2Statements() {
    return {
        // Distinguish a genuinely running job from a row left behind by a
        // crashed process, so stale runs can be recovered instead of blocking
        // future runs.
        QStringLiteral("ALTER TABLE job_runs ADD COLUMN owner_pid INTEGER"),
        QStringLiteral("ALTER TABLE job_runs ADD COLUMN owner_host TEXT NOT NULL DEFAULT ''"),

        // A source label gives every source its own namespace on the remote,
        // so two sources with identical file names cannot collide.
        QStringLiteral("ALTER TABLE backup_sources ADD COLUMN label TEXT NOT NULL DEFAULT ''"),

        // Why a file was not transferred (incremental skip, exclusion, ...).
        QStringLiteral("ALTER TABLE file_manifest ADD COLUMN skip_reason TEXT"),

        // At most one running run per job: the database enforces the
        // duplicate-execution guard instead of trusting the UI.
        QStringLiteral("CREATE UNIQUE INDEX ux_job_runs_single_running "
                       "ON job_runs(job_id) WHERE status = 'running'"),

        QStringLiteral("CREATE INDEX idx_archive_objects_job ON archive_objects(job_id)"),
        QStringLiteral("CREATE INDEX idx_restore_jobs_job ON restore_jobs(job_id)"),
    };
}

QStringList migrationV3Statements() {
    return {
        // Persistent retry state: a failed run may be retried after a backoff,
        // and that intent must survive an agent restart.
        QStringLiteral("ALTER TABLE job_schedules ADD COLUMN retry_attempt_count INTEGER NOT NULL "
                       "DEFAULT 0"),
        QStringLiteral("ALTER TABLE job_schedules ADD COLUMN retry_next_at TEXT"),

        // The scheduler asks for the earliest due schedule on every tick.
        QStringLiteral("CREATE INDEX idx_job_schedules_next_run ON job_schedules(next_run_at)"),
    };
}

QStringList migrationV4Statements() {
    return {
        // The snapshots page shows how many files each snapshot holds.
        QStringLiteral("ALTER TABLE backup_snapshots ADD COLUMN files_processed INTEGER"),
    };
}

QStringList migrationV5Statements() {
    return {
        // Message counts per folder, so a sync can state how much work is ahead
        // before it downloads anything.
        QStringLiteral("ALTER TABLE mail_folders ADD COLUMN total_item_count INTEGER NOT NULL "
                       "DEFAULT 0"),

        // The archived size, measured from the .eml itself: the delta feed does
        // not return a message size.
        QStringLiteral("ALTER TABLE mail_messages ADD COLUMN size_bytes INTEGER NOT NULL "
                       "DEFAULT 0"),

        // Speed of the last sync, so the next run can estimate its duration.
        QStringLiteral("ALTER TABLE mail_accounts ADD COLUMN last_sync_messages INTEGER NOT NULL "
                       "DEFAULT 0"),
        QStringLiteral("ALTER TABLE mail_accounts ADD COLUMN last_sync_bytes INTEGER NOT NULL "
                       "DEFAULT 0"),
        QStringLiteral("ALTER TABLE mail_accounts ADD COLUMN last_sync_seconds REAL NOT NULL "
                       "DEFAULT 0"),
    };
}

} // namespace

const QStringList& bootstrapStatements() {
    static const QStringList statements = {
        QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations (
    version    INTEGER PRIMARY KEY,
    name       TEXT NOT NULL,
    applied_at TEXT NOT NULL
))SQL"),
    };
    return statements;
}

const std::vector<Migration>& allMigrations() {
    static const std::vector<Migration> migrations = {
        Migration{
            1,
            QStringLiteral("initial-schema"),
            QStringLiteral("Baseline schema: settings, storage, jobs, runs, manifests, mail."),
            migrationV1Statements(),
        },
        Migration{
            2,
            QStringLiteral("archive-execution"),
            QStringLiteral("Execution support: run ownership, source labels, skip reasons, "
                           "single-running-run guard and lookup indexes."),
            migrationV2Statements(),
        },
        Migration{
            3,
            QStringLiteral("scheduling-state"),
            QStringLiteral("Scheduling: persistent retry state and the next-run index used by "
                           "the background agent."),
            migrationV3Statements(),
        },
        Migration{
            4,
            QStringLiteral("snapshot-file-counts"),
            QStringLiteral("Restic snapshots record how many files they contain."),
            migrationV4Statements(),
        },
        Migration{
            5,
            QStringLiteral("mail-progress-and-sizes"),
            QStringLiteral("Mail: per-folder message counts, archived sizes and the speed of the "
                           "last sync, so progress and duration can be reported."),
            migrationV5Statements(),
        },
    };
    return migrations;
}

int latestSchemaVersion() {
    const auto& migrations = allMigrations();
    return migrations.empty() ? 0 : migrations.back().version;
}

const Migration* findMigration(int version) {
    for (const Migration& migration : allMigrations()) {
        if (migration.version == version) {
            return &migration;
        }
    }
    return nullptr;
}

} // namespace nesva::db
