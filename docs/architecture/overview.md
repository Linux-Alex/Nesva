# Nesva architecture overview

Status: Phase 1 implemented (foundation). This document describes the target architecture and
marks precisely which parts exist today.

---

## 1. Product model

Nesva has two independent modes that must never be conflated in code, storage layout, UI or
documentation.

### Archive mode
Copies ordinary files (documents, photos, user-selected directories, exported `.eml` mail) to a
destination that remains usable outside Nesva. Files stay browsable through the provider.

- Semantics: **copy**. The source tree is never deleted, moved or rewritten.
- No version history, no encryption layer of its own.
- Never described as a versioned or immutable backup.

### Backup mode
Creates versioned, optionally encrypted snapshots with Restic, with retention policies,
integrity checking and restore.

- Semantics: snapshots plus retention; the repository is Restic's, not a synced folder.
- A repository is never placed inside a general-purpose sync folder and assumed consistent.

---

## 2. Component map

```text
                       ┌──────────────────────────────────────────┐
                       │             nesva-desktop                │
                       │        Qt 6 Widgets (no business logic)  │
                       │  pages ─ models ─ AppContext (services)  │
                       └───────────────┬──────────────────────────┘
                                       │  in-process calls
┌──────────────────────────────────────▼───────────────────────────────────────┐
│                              nesva-core (C++20)                              │
│  job orchestration · selection/exclusions · configuration · progress ·        │
│  structured logging · error model · cancellation · retry · database ·        │
│  restore coordination · storage-provider abstraction                          │
└───────┬───────────────────┬──────────────────────┬───────────────────────────┘
        │                   │                      │
        ▼                   ▼                      ▼
┌───────────────┐   ┌───────────────┐      ┌────────────────────┐
│ providers/    │   │ database/     │      │ external tools     │
│ local, rclone │   │ SQLite +      │      │ rclone, restic via │
│ (Phase 2)     │   │ migrations    │      │ QProcess (no shell)│
└───────────────┘   └───────────────┘      └────────────────────┘
        ▲                   ▲                      ▲
        │                   │                      │
┌───────┴───────────────────┴──────────────────────┴───────────────────────────┐
│ nesva-cli (automation/diagnostics)      nesva-agent (Phase 3: scheduled runs)│
└──────────────────────────────────────────────────────────────────────────────┘
```

Rules enforced by the build:

- `nesva-core` links Qt Core and Qt SQL only. It never includes Qt Widgets, so business logic
  cannot drift into the UI.
- `nesva_ui` (static library) contains the widgets and depends on `nesva-core`.
- `nesva-cli` depends on `nesva-core` only, so it works without a display server.
- The desktop executable is a thin `main()` that wires application metadata, theme and the
  window.

---

## 3. Layers and responsibilities

### 3.1 nesva-core (`src/core`, `include/nesva`)

| Module | Responsibility | Phase 1 |
|---|---|---|
| `core/error.hpp`, `core/result.hpp` | Error codes, `Error`, `Result<T>`, `Status`; no exceptions for expected failures | done |
| `core/logging.hpp` | Structured records, sinks (file/stream), rotation, level filtering | done |
| `core/redactor.hpp` | Credential censoring before any sink or export sees a record | done |
| `core/paths.hpp` | Per-user data/log/cache resolution with overrides | done |
| `core/app_info.hpp` | Version, Qt version, platform, build metadata | done |
| `core/external_tool.hpp` | Safe external process probing (path, version, exit code, timeout) | done |
| `db/database.hpp` | Connection, PRAGMAs, transactions, integrity checks | done |
| `db/migrations.hpp` | Forward-only versioned schema | done (v1) |
| `config/settings_store.hpp` | Typed persisted settings | done |
| job orchestration, selection, providers, retry, cancellation | Phase 2+ | pending |

### 3.2 nesva_ui (`src/desktop`)

- `AppContext` owns the database connection, settings store, logging configuration and theme
  state; pages receive it by reference and never open their own connections.
- `MainWindow` builds the shell (top bar, sidebar, page stack, activity dock, tray, status bar)
  and drives refresh on activation, on a timer, and on menu/shortcut actions.
- `Page` subclasses own their widgets; `refresh()` reloads from the database on demand.
- `JobListModel` / `EventListModel` are `QAbstractTableModel` implementations over SQL; the
  views never touch SQL directly.
- Long-running work in Phase 1 is limited to short subprocess probes (Rclone/Restic version
  checks, ~50 ms) and SQL queries; the UI stays responsive, and heavier work moves off the GUI
  thread when job execution lands.

### 3.3 nesva-cli (`src/cli`)

- Argument parsing without third-party dependencies; `--json` for automation.
- Exit codes are a contract: `0` success, `1` usage, `2` runtime failure, `3` not found,
  `4` not implemented.
- Commands that are not part of this phase fail loudly instead of silently doing nothing.

### 3.4 nesva-agent

An independent `QCoreApplication` process that schedules and runs archive jobs without the
desktop window:

- tick loop (default 20 s) that evaluates schedules and starts at most one job at a time;
- missed-run catch-up (once, with a 7-day age limit) and persistent retries with geometric
  backoff;
- per-job maximum duration enforced by cancelling the worker;
- graceful SIGINT/SIGTERM shutdown that finishes the file in flight; SIGHUP reloads schedules;
- single-instance lock file and a heartbeat file (`agent.state.json`);
- a local JSON socket (Unix socket / named pipe) for `status`, `run`, `cancel`, `reload` and
  `shutdown`, shared by the desktop application and `nesva-cli`;
- OS registration: systemd **user** unit on Linux, per-user scheduled task on Windows
  (`LogonType Interactive`, `RunLevel Limited`) - never a SYSTEM service, because SYSTEM cannot
  see a user's cloud credentials or keyring.

Schedule arithmetic lives in `nesva::sched` as pure functions (unit-tested without a clock or a
database); see ADR 0011.

---

## 4. Data model

SQLite, WAL journal, `foreign_keys=ON` per connection, forward-only migrations recorded in
`schema_migrations`.

```text
settings                     key/value store with type tag
storage_providers            destination definition + capability/status snapshot
storage_credentials_refs     pointers into an OS keyring; never a secret
backup_jobs                  mode ('archive' | 'backup'), destination, policies
backup_sources               one row per source directory (FK cascade)
backup_exclusions            glob/regex/literal exclusion patterns
job_schedules                trigger kind, timing, retry and catch-up policy
job_runs                     per-execution counters, status, error
job_events                   structured per-run events
file_manifest                per-run file inventory with hash state
archive_objects              archive-mode remote objects and verification state
backup_snapshots             Restic snapshot index
restore_jobs                 restore requests, conflict policy, counters
mail_accounts                Microsoft 365 account metadata (Phase 5)
mail_folders                 mailbox/folder selection and delta links
mail_messages                stable message identity and EML export state
application_events           application-level diagnostic history
schema_migrations            applied migration versions
```

Invariants encoded in the schema, not just in code:

- `backup_jobs.copy_semantics` is constrained to `'copy'`.
- `backup_jobs.allow_remote_deletions` defaults to `0`.
- `storage_credentials_refs` stores a `secret_reference` and a `keyring_backend`; there is no
  column for a plaintext password.
- Timestamps are ISO-8601 UTC strings (`...Z`), comparable lexicographically.

---

## 5. Storage providers

Provider abstraction (Phase 2) with an explicit capability model, because cloud providers differ:

```text
can_list, can_upload, can_download, can_delete, can_rename, can_verify_hash,
supports_atomic_move, supports_versioning, requires_interactive_auth
```

Implementations planned: local filesystem, SFTP, S3-compatible, WebDAV, and Rclone-backed remotes
(including iCloud Drive). iCloud is a supported provider, not a dependency: no custom Apple API
client is implemented, authentication happens locally through Rclone, and an upload is only
reported complete when the provider confirms it - never because a local copy exists.

### 5.1 Rclone usage rules (Phase 2)

- Arguments passed as an array through `QProcess`; no shell, no string concatenation.
- Paths with spaces and Unicode are preserved; `stdout` and `stderr` captured separately.
- Machine-readable output (JSON where available) is preferred over parsing human progress.
- Cancellation, exit-code detection, retries with backoff, bandwidth limits, connection tests,
  and job-level resume.
- Trust tokens and Apple passwords never reach logs; the redactor runs before every sink.

### 5.2 Restic usage rules (Phase 4)

Repository initialisation, backup, snapshot listing, restore, verification, retention preview and
execution, lock detection, and password handling through the OS keyring. Exclusive repository
access is required while mutating a repository.

### 5.3 External tool policy

- Detect installed binaries (PATH or user-configured absolute path), check minimum versions.
- Never download or execute arbitrary unverified binaries at runtime.
- Licences are reviewed and documented; bundling, if it ever happens, is pinned and checksummed.

---

## 6. Scheduling (Phase 3)

Daily, weekly, monthly, startup-triggered and manual runs; missed-run handling; retry policy;
maximum duration; network availability checks. Execution state is persisted in SQLite so a
reboot or a closed GUI cannot lose a run. The GUI only displays schedule state; the agent owns
execution.

---

## 7. Microsoft 365 mail archival (Phase 5)

Microsoft Graph with delegated OAuth (authorisation-code + PKCE, public client), least-privilege
scopes, tokens in the OS keyring, MIME `.eml` export through Graph's MIME endpoint, stable
message identity (`internetMessageId` + mailbox), delta-based incremental sync, restartable
without duplication, and never deleting mail from Microsoft 365.

---

## 8. Threading and responsiveness

- Phase 1: all database access and UI work happens on the GUI thread; the only blocking calls
  are short bounded subprocess probes with explicit timeouts.
- Phase 2+: job execution moves to worker threads (`nesva-core` is thread-agnostic; one
  `Database` instance per thread), with progress marshalled to the UI.
- The logger is thread-safe; sinks serialise their own writes.

---

## 9. Error handling and diagnostics

- Every fallible operation returns `Result<T>` or `Status`; errors carry a stable code
  (`invalid_argument`, `not_found`, `external_tool_failed`, `integrity_error`, ...).
- Errors are logged with structured fields and persisted to `application_events` /
  `job_events` so the Activity page and `nesva-cli` show the same history.
- `nesva-cli doctor` verifies: data directory writability, log directory writability, database
  integrity, schema version, free disk space, Rclone/Restic availability.

---

## 10. Filesystem and platform behaviour

Windows-specific hazards to be handled in Phase 2 execution code (documented now so the design
accounts for them):

- Long paths, Unicode names, case-insensitive collision handling.
- Junctions, reparse points, symlink loops, cloud-file placeholders.
- Locked files and files changing during a transfer.
- No VSS in the first implementation: a transfer is **not** a consistent point-in-time snapshot,
  and the documentation must say so.

Linux: symlinks (no following by default), permissions and extended attributes, mount-point
boundaries, case-sensitive paths, hidden files and special files.

Both: recursion protection must exclude the destination directory, sync folders, and any path
that would re-enter the source tree.

---

## 11. Security posture

- No hardcoded credentials; no credentials in Git; no credentials in logs (`Redactor`).
- Least privilege for every OS integration; safe subprocess invocation without a shell.
- Secure temporary files inside the per-user cache directory, bounded in size.
- Secrets belong in Windows Credential Manager / Linux Secret Service; SQLite stores only
  references.
- Telemetry: none. Nothing leaves the machine unless the user configures a destination.
- No custom cryptography is implemented; Restic provides encryption.

---

## 12. Phase status

| Phase | Deliverable | State |
|---|---|---|
| 1 | Build system, core services, SQLite + migrations, logging, desktop shell, CLI, tests | delivered |
| 2 | Job editor, local + Rclone providers, execution, progress, history, restore | delivered |
| 3 | Agent, IPC, scheduling, missed runs, tray, notifications | delivered |
| 4 | Restic repository, snapshots, restore, retention, verification | next |
| 5 | Microsoft 365 OAuth, mailbox selection, EML export, incremental sync | planned |
| 6 | Windows installer, Linux packages, release CI, user/security documentation | planned |

### 12.1 Phase 2 execution path

```text
JobEditorDialog ──save──▶ JobStore (backup_jobs / sources / exclusions)
      │ dry-run                        │
      ▼                                ▼
JobExecutor (GUI thread) ──spawn──▶ Worker QThread ── own Database connection
                                            │
                                            ▼
                                   ArchiveRunner
                                     ├── scanSources (exclusions, recursion guards)
                                     ├── lastSuccessfulManifest (incremental decision)
                                     ├── StorageProvider::uploadFile (local | rclone)
                                     └── file_manifest / archive_objects / job_events / counters
                                            │
                                            ▼
                         main window: progress bar, activity dock, history
```

Restore reverses the same path: `RestorePage` → `ArchiveRestorer` → provider download →
post-restore hash comparison against the manifest → `restore_jobs`.
