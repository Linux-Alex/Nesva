# Changelog

All notable changes to Nesva are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Rclone installation from inside Nesva.** Settings -> External tools -> *Install Rclone...*
  (CLI: `nesva-cli tools rclone`) resolves the current release and shows every step before
  anything happens: the download URL, the published SHA-256 check, the exact unpack command, the
  target path and, for an all-users install, the full elevated command
  (`pkexec install -m 0755 ...`). The archive is verified against the publisher's `SHA256SUMS`
  before it is unpacked and nothing is installed on a mismatch; the default scope is per-user and
  needs no privileges; there is no shell anywhere, and the archive is never unpacked as root. The
  installed path is recorded so Rclone works without changing `PATH`.
- **Mail progress you can plan with.** `nesva-cli mail estimate <account>` counts every folder
  from Microsoft's own counters and prints what is left; a sync now reports
  `[ 42.3%] 2053/4881 messages · 7.9 msg/s · ETA 6m 0s · 1.2 GB archived`, switching to `[delta]`
  for incremental runs where a percentage of the whole mailbox would be misleading. Per-folder
  counts, archived sizes and the measured speed of the last sync are stored (schema v5), so the
  next estimate is based on real throughput rather than a guess.
- **`docs/user-guide/tutorial.md`**: a guided walkthrough from environment check to a verified
  archive, with the acceptance scenario to run after any change.

- **Actionable Microsoft Entra diagnostics.** A rejected token request now leads with the fix for
  the `AADSTS` code (public client flows disabled, redirect URI mismatch, missing consent,
  account not assigned, expired or reused code, single-page-app misconfiguration) and keeps
  Microsoft's raw text and trace id after it. The desktop client never suggests supplying a
  client secret.
- **`docs/user-guide/microsoft-365.md`**: user-scoped app registration walkthrough (including the
  `Allow public client flows` requirement), connection, sync semantics and a troubleshooting
  table for the `AADSTS` codes encountered in practice.

### Fixed

- **Mail sync survives a real mailbox.** Four defects found by the first live run against a
  tenant, none of which the scripted tests could see:
  - `QNetworkHttpTransport` built a `QNetworkAccessManager` per request, paying a DNS+TCP+TLS
    handshake for every call (measured at ~235 ms each, one to two calls per message). The manager
    is now held in a `thread_local`, keeping the connection pool per thread: ~68 ms per request,
    a 1.1 MB message in ~144 ms, and each request logs host, path, status, bytes and elapsed time
    at debug level.
  - The delta `$select` asked for `size`, which Exchange rejects outright
    (`RequestBroker--ParseUri`), failing every folder before anything was downloaded.
  - A run bounded by `--max-messages` advanced the delta cursor past messages it never examined,
    so Microsoft never reported them again and the archive silently missed them. A truncated page
    no longer moves the cursor, and the run stops instead of visiting every remaining folder.
  - "Already exported" ignored the file on disk, so a new destination produced an empty archive
    and a deleted `.eml` was never restored. The recorded path is now checked under the current
    destination.
- **HTTP transport discarded every error response.** Qt reports an HTTP 4xx/5xx as a
  `QNetworkReply` error, and the transport treated any reply error as "no HTTP response": the
  status code collapsed to 0 and the body was dropped. Consequences fixed: a rejected token
  request now reports the real AAD error instead of `0:`; Graph 401 token refresh, 429
  `Retry-After` throttling and 5xx backoff now receive the status they act on. Also
  distinguishes a real transport failure (no status) from a server error, and reports timeouts
  as timeouts instead of a bare cancellation.

## [0.5.0] - 2026-11-02

Phase 5 (in progress): Microsoft 365 mail archival. The engine, the CLI and the tests are in
place; the desktop UI for accounts and scheduling is the remaining work.

### Added

- **OAuth 2.0 with PKCE** for a public desktop client: loopback redirect listener (IPv4+IPv6,
  ephemeral port), CSRF state check, AAD error surfacing, and the `AADSTS50011` fallback for
  tenants that match the redirect URI without a port.
- **Token handling**: refresh token in the OS keyring, access token in memory only, rotated
  refresh tokens persisted immediately.
- **Graph client** (read-only): profile, folder tree (with child folders), per-folder message
  delta feed, and the MIME content endpoint. 401 retry-after-refresh, 429 `Retry-After` handling,
  5xx backoff.
- **Incremental EML exporter**: per-folder delta cursor stored in SQLite and advanced page by
  page, so an interrupted run resumes; unchanged messages are skipped by
  `lastModifiedDateTime`; `@removed` keeps the local file and marks the row. Files:
  `<folder>/<YYYY>/<MM>/<subject>-<id12>.eml` written atomically with SHA-256 recorded.
- **CLI**: `mail connect`, `mail accounts`, `mail folders` (with `--enable/--disable`),
  `mail sync` (with `--to`, `--full`, `--folder`, `--max-messages`) and `mail status`.
- **Tests**: PKCE against the RFC 7636 vector, URL/token parsing, filename sanitisation, path
  building, and a full export run against a scripted Graph (paging, dedupe on rerun, removed
  messages, HTTP errors).

### Known limitations

- The desktop UI for Microsoft 365 (connect button, account list, folder selection, scheduled
  mail archival) is not wired yet; use `nesva-cli mail` for now.
- Live verification needs an interactive sign-in: the automated tests use a scripted Graph
  transport, and the first real sync is executed against the configured tenant by the user.
- Windows remains unbuilt/unverified.

## [0.4.0] - 2026-11-02

Phase 4: Restic backup engine, credential storage and the matching desktop UI. Everything below
is implemented and tested against real Restic repositories.

### Added

- **Restic integration** (`nesva::backup`): repository init, backup with live JSON progress,
  snapshot listing, snapshot file listing, restore, retention preview/apply (with optional
  prune), repository check with `--read-data-subset`, lock listing and unlock. Restic exit codes
  10/11/12 map to NotFound/AlreadyExists/PermissionDenied.
- **Credential store** (`nesva::security`): freedesktop Secret Service backend over D-Bus for
  Linux, Windows Credential Manager backend, and an explicit "unavailable" backend that reports
  why instead of silently storing plaintext.
- **Repository passwords** are passed to restic through a 0600 temporary file created in the
  cache directory and removed immediately; the keyring account is derived from the repository
  location.
- **Backup jobs** in the job store: mode `backup` with a repository location, retention policy
  and post-backup verification flag, plus snapshot records in `backup_snapshots`.
- **BackupRunner**: resolves the password, runs restic, records run counters/snapshot/events,
  supports dry-run (connectivity check), cancellation and optional post-backup `check`.
- **BackupRestorer**: stages the restore and merges with skip/rename/overwrite policies, moving
  files (no double copy) and stripping the snapshot's common root.
- Desktop: job editor with Backup mode (repository location, retention policy, post-backup
  verification, "Set password..." and "Initialise repository..."); Snapshots page with
  "Refresh from repository", snapshot restore dialog, retention preview/apply (optional prune),
  repository check and unlock; archive and backup jobs both run through the same worker, so
  scheduled and manual runs behave identically.
- CLI: `repository init|check|locks|unlock|set-password`, `snapshots list`, `retention
  preview|apply`, `backup restore`, and `jobs run` now dispatches on the job mode.
- Tests: `tst_secret_store` (real keyring round-trip, skippable when unavailable),
  `tst_restic` (argv + parsers + real repository lifecycle/retention/check/restore) and
  `tst_backup_runner` (job-level backup, incremental, cancellation, dry-run, conflict policies).
  21 binaries, 222 test functions.

### Fixed

- The streaming restic runner consumed stdout for line callbacks and left the buffer empty for
  parsers; it now accumulates output as well.
- Restore no longer leaves files under the snapshot's original absolute path inside the target.

### Known limitations

- Windows remains unbuilt/unverified.
- rclone-backed repositories are supported by construction (`rclone:<remote>:<path>`) but were
  not exercised, because Rclone is not installed in this environment.

## [0.3.0] - 2026-11-02

Phase 3: background execution. Scheduled jobs run without the desktop window.

### Added

- **`nesva-agent`** executable: schedule loop (default 20 s tick), one job at a time,
  missed-run catch-up (once, with an age limit), persistent retries with geometric backoff,
  per-job maximum duration, graceful SIGINT/SIGTERM/SIGHUP handling, single-instance lock file
  and a heartbeat file (`agent.state.json`).
- Schedule model (`nesva::sched`): daily/weekly/monthly/on-login/on-startup triggers,
  timezone-aware next-run computation that survives DST gaps, due/missed/retry evaluation and
  outcome planning — all pure functions, unit-tested without a clock or database.
- Local IPC: line-oriented JSON over `QLocalServer` (`status`, `run`, `cancel`, `reload`,
  `shutdown`), namespaced per data directory, with a shared `AgentClient` used by the desktop
  application and the CLI.
- Schema migration v3: persistent retry state and the next-run index.
- Desktop integration: agent indicator in the status bar, agent card on the Settings page
  (status, refresh, stop), "Run now" delegated to the agent when it is reachable, schedule
  editor in the job dialog with a live next-run preview, "Next run" column in the jobs table,
  optional minimise-to-tray, desktop notifications for finished runs.
- CLI: `agent status|run|cancel|reload|stop` and `schedule list`.
- Packaging: systemd **user** unit (`packaging/linux/nesva-agent.service`), installer script,
  and a per-user Windows scheduled-task script (`LogonType Interactive`, `RunLevel Limited` —
  never SYSTEM).
- Documentation: `docs/developer-guide/agent.md` (operation, IPC protocol, troubleshooting).
- Tests: schedule logic, agent protocol/heartbeat, and an integration test that starts the real
  `nesva-agent` binary, talks to it over its socket and verifies a scheduled run — 18 binaries,
  188 test functions.

### Fixed

- The agent's socket handler now drains data that arrived before `readyRead` was connected; a
  local client writes immediately, which previously left the request unread (found by the
  integration test).
- `AgentClient` without an explicit socket name now resolves the name from the current data
  directory instead of using an empty (invalid) name.

### Known limitations

- Windows has not been built or tested; the scheduled-task script is unverified.
- Network-availability checks before a run are not implemented yet.
- Cloud (Rclone) transfers are still only covered by unit tests with a scripted runner.

## [0.2.0] - 2026-11-02

Phase 2: functional archive. Archive jobs can be created, executed, inspected and restored.

### Added

- `nesva-core` archive engine: file scanner with exclusion patterns, symlink and depth
  protection, destination exclusion and skip reasons; exclusion rule compiler (glob/regex/literal).
- Storage provider abstraction with an explicit capability model, plus two implementations:
  - local filesystem provider: atomic writes (temporary file + rename), size and SHA-256
    verification, mtime preservation, path-traversal rejection;
  - Rclone provider: argument arrays, `copyto`/`lsjson`/`mkdir`/`hashsum`, JSON parsing,
    retries with backoff, cancellation, and an injectable runner for deterministic tests.
- Archive runner: incremental runs driven by the previous successful manifest, per-file manifest
  and archive-object records, structured job events, counters, cancellation, a fatal-vs-file-local
  failure policy, and a dry-run mode that writes nothing.
- Archive restore: planning with conflict detection, conflict policies (skip/rename/overwrite, with
  "ask" explicitly refused), post-restore verification against manifest hashes, and a
  `restore_jobs` record per attempt.
- Job store: CRUD for jobs, sources, exclusions and storage providers, a run lifecycle with
  database-enforced single flight, and crash recovery of interrupted runs.
- Schema migration v2: run ownership (pid/host), source labels, manifest skip reasons, the
  single-running-run unique index and lookup indexes.
- Desktop: job editor (sources, destination, exclusions, bandwidth, verification, dry-run
  preview); Jobs page actions (new/edit/delete/run/cancel/history); Archive Explorer; Restore
  wizard with live progress; storage connection manager with add/test/remove; worker-thread
  execution so the window stays responsive.
- CLI: `jobs run` (`--dry-run`, `--force`, `--retries`), `jobs cancel`, `storage test`,
  `archive list`, `archive restore` and `verify` for archive manifests.
- Tests: seven new suites (exclusions, scanner, job store, local provider, rclone provider,
  archive runner, archive restore) plus worker-thread execution coverage in the GUI suite;
  15 binaries, 150 test functions.

### Safety

- Archive jobs are copy-only, and the destination is excluded from scanning so a job cannot
  archive its own output.
- Cancellation never reports success: a cancelled scan or transfer finishes the run as
  `cancelled`.
- A failed file does not abort the run unless the destination itself is unusable (three
  consecutive failures or a destination-level error).
- Restore never overwrites silently: `Ask` is rejected before the first write.

### Known limitations

- Rclone transfers are covered by unit tests against a scripted runner; no live cloud transfer was
  executed in this environment (no Rclone install, no iCloud account).
- Windows has not been built or tested.
- No scheduling or background agent yet (Phase 3); runs are started manually.
- Restic backup mode still reports "not implemented" (Phase 4).

## [0.1.0] - 2026-11-02

Phase 1: foundation. No backup or archive transfer is executed in this release.

### Added

- CMake project (C++20, Qt 6 Widgets) with `CMakePresets.json` for Linux GCC/Clang, ASan and
  MSVC, warnings-as-errors, CTest integration and CPack configuration.
- `nesva-core` library: error/`Result` model, structured JSON logging with rotation and
  per-sink levels, credential redaction, per-user path resolution, external tool probing,
  SQLite layer with WAL/foreign keys/transactions/savepoints/integrity checks, versioned
  migrations (schema v1, 17 tables), typed settings store.
- `nesva` desktop application: sidebar navigation, status bar, activity dock, system tray,
  eight pages (Dashboard, Backup Jobs, Storage, Archive Explorer, Snapshots, Restore, Activity,
  Settings), light and dark themes, programmatic theme-aware icons, keyboard shortcuts,
  persisted window state, and Dashboard/Activity data from the real database.
- `nesva-cli` with `status`, `doctor`, `jobs list`, `storage list`, `history list`, `--json`
  output and documented exit codes; un-implemented commands fail with exit code 4 and an
  explicit phase message.
- 8 test binaries covering results, redaction, logging, paths, database, settings, external
  tools and a headless GUI smoke test; all registered with CTest.
- `nesva --gui-selftest` mode that captures one screenshot per page, usable headless or on X11.
- Documentation: architecture overview, nine ADRs, developer build guide, security policy,
  contributing guide.

### Security

- Credential redaction is applied to every log record before any sink writes it.
- Destructive defaults encoded in the schema: copy-only archive semantics and remote deletions
  disabled.
- No credentials, no network access and no subprocess execution of untrusted binaries in this
  release.

### Known limitations

- Windows was not built or tested during this phase; Windows presets and CI workflow are
  provided but unverified.
- No job creation, execution, scheduling, Restic or Microsoft 365 support yet.
- Rclone and Restic are not present in the development environment; `doctor` reports them as
  warnings.
