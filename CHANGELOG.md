# Changelog

All notable changes to Nesva are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added (external tools)

- **Restic can now be installed from Nesva too.** The installer was rclone-only, so a machine
  without restic could not create backups without leaving the app. **Settings -> External tools**
  now has **Install Restic...** next to **Install Rclone...**, and `nesva-cli tools restic`
  (preview) / `nesva-cli tools restic --install` do the same from a terminal. The resolver reads
  the current release from the GitHub API, downloads the official asset (zip on Windows, bzip2 on
  Linux and macOS), verifies the published SHA-256 from `SHA256SUMS` before unpacking, installs the
  single binary into the per-user program directory and records its absolute path. Both tools share
  one code path (`tools::planToolInstall`/`executeToolInstall`) so the preview cannot drift from
  what runs.

### Added (cloud credentials)

- **Provider tutorials in the rclone wizard.** The dialog now has a provider selector
  (iCloud Drive, Google Drive, pCloud, Backblaze B2, OneDrive, WebDAV/NAS); choosing one swaps the
  walkthrough on the left and the suggested remote name, and the quick-answer buttons offer the
  backend names at rclone's `Storage>` prompt. Each tutorial includes where the credentials come
  from (Google Cloud OAuth client, Backblaze application key, pCloud sign-in).


- **Guided rclone configuration** (`RcloneConfigDialog`): Storage ->
  **Configure / re-authenticate...** opens a split view - the walkthrough on the left, a live
  `rclone config` session on the right with the transcript, an answer line and quick-choice
  buttons parsed from rclone's own prompts. The Apple ID password and the two-factor code are
  typed there and go straight into rclone's configuration; Nesva never stores them.
- **Stale cloud sessions are detected once, not per file.** Before a run with an rclone-backed
  provider, Nesva probes the destination; an authentication failure marks the provider
  `auth_required` (the dashboard and storage list show it) and stops the run with "re-authenticate
  and enter a fresh two-factor code" instead of failing every file.

### Added (UI polish)

- **Dashboard cards flow instead of shrinking.** A `FlowLayout` wraps them onto further rows when
  the window is narrow, and the value labels wrap too, so a long timestamp breaks onto a second
  line instead of being clipped.
- **Display timezone.** Settings -> Appearance -> *Time zone* selects the zone used for every
  timestamp in the UI (dashboard cards, recent events, activity). Stored values stay UTC.
- **In-app Entra help.** Settings -> Microsoft 365 -> *How to register the app...* opens the full
  registration walkthrough (platform, redirect URI, *Allow public client flows*, permissions and
  the `AADSTS` codes), so setup does not depend on having the documentation at hand.

### Fixed (UI polish)

- **Every answer in the rclone wizard was sent twice.** A `QPushButton` in a dialog is
  "auto default", so pressing Enter in the answer line fired both `returnPressed` and the button,
  writing two newlines: rclone then reported "Bad password: no characters in password" and re-asked
  every question. The dialog's buttons no longer take the default role.
- **Job list ID column.** The model returned the string "right" for `Qt::TextAlignmentRole`, which
  Qt ignores: the ID was drawn top-left in a cramped cell while other columns looked normal. It
  now returns a real `Qt::Alignment` (right + vertically centred), and the ID and source-count
  columns size to their contents.

### Added (17-item pass for the Windows deployment)

- **Microsoft 365 in the GUI.** Settings -> *Microsoft 365* holds the Entra application details
  (tenant id, client id, default archive folder) and a **Connect account** dialog that runs the
  browser sign-in off the UI thread, shows the sign-in URL, and writes the refresh token to the
  keyring. The shared `mail::connectAccount` makes the CLI and the dialog behave identically.
- **Mail page fixes**: the folder *Include* checkbox now persists through
  `MailStore::setFolderSyncEnabled` (it was checkable but ignored), an expired sign-in offers
  "Connect again" instead of a dead end, and the job list shows mail jobs as `mail` / `mailbox`.
- **Cloud placeholder protection** (`src/scan/file_scanner.cpp`): files carrying
  `FILE_ATTRIBUTE_OFFLINE`/recall attributes are skipped with a recorded reason instead of being
  hydrated, and cloud sync roots (iCloud Drive, OneDrive, Dropbox, ... - reparse points on Windows
  plus known names) are not walked at all. Both default to on.
- **Identity guard** (`src/security/identity.cpp`): the process reads its own token/SID. Running as
  SYSTEM/LocalService/NetworkService (or root) is refused before a job starts, with an explanation;
  `nesva-cli doctor` reports the user context.
- **Destination safety**: a job refuses to write into a destination that already contains files
  Nesva did not create, unless the job explicitly allows it (schema v7,
  `allow_existing_destination`). This is what keeps a wrong path from overwriting existing cloud
  data.
- **Upload verification**: after each upload the provider is asked for the stored SHA-256 and the
  file is recorded as `verified` only on a match (`archive_runner`); a mismatch fails that file
  instead of reporting success.
- **Disk-space guard with relocation advice** (`src/core/disk_space.cpp`): a local destination is
  checked before the transfer, and the error names another volume with room instead of failing
  halfway.
- **Manifest pruning**: the last 5 runs per job are kept, older manifest and object rows are deleted
  after a successful run, and `nesva-cli maintenance` prunes on demand.
- **Agent autostart without PowerShell**: `nesva-cli agent install|uninstall` and the Settings card.
  Linux writes a systemd user unit and enables it; Windows writes the per-user
  `HKCU\\...\\CurrentVersion\\Run` value - no PowerShell, no `schtasks`, no elevation, so
  script-blocking group policies do not apply. The PowerShell registration script is gone.
- **`nesva-cli rclone configure`** and a **Configure Rclone...** button on the Storage page open
  `rclone config` in a terminal, because it is inherently interactive.
- **`nesva-cli repository passwd`**: rotates a Restic repository password (add key, prove the new
  password opens the repository, only then remove the old key, then update the keyring).
- **CPack**: `nesva-agent` joins the install set; Windows packages with NSIS/ZIP instead of WIX.
- **`docs/user-guide/disaster-recovery.md`**: what to keep, how to restore without Nesva, and what
  is not recoverable.
- **`tst_safety`**: service-account refusal, cloud placeholder skip, cloud root skip, disk-space
  guard. Test suite: 24 binaries.

- **Mail archival is a job.** A job can now take its content from a Microsoft 365 mailbox
  (`source_kind = 'mail'`, schema v6), so scheduled mail archiving gets the scheduler, retries,
  run history, notifications and the agent's single-run guard without a special case.
  `nesva-cli mail job <account> --to <dir> --schedule daily --time 03:00` creates it; the desktop
  **Mail** page creates the same job and can start it with **Sync now**.
- **Mail page** (new, Ctrl+5): connected accounts, per-folder counts with an include checkbox,
  **Estimate** with a duration once a speed has been measured, **Sync now**, **Schedule job...**
  and **Back up archive with Restic...**, which creates a backup job over the archive directory -
  the versioned, encrypted counterpart of the plain `.eml` copy.
- **Job editor: "Content" selector** - *Files and folders* or *Microsoft 365 mailbox* - with the
  account picker and the mail-specific validation (an account and a destination directory).
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

- **Status lines no longer move the job table.** A long run message wrapped, grew its row and
  pushed the Backup Jobs table down while a run was in progress; the error line appeared and
  disappeared with the same effect. Status and error lines are now single-line labels that elide
  with the full text in a tooltip, so the table geometry never changes. The same fix applies to the
  Storage and Snapshots pages.
- **Settings content was unreachable on a normal-height window.** Page bodies were a plain
  vertical layout, so a page with more cards than the window is tall compressed every card below
  its natural size: labels wrapped into unreadable slivers and the later sections - including the
  new External tools card - simply could not be reached. Every page body now lives in a vertical
  scroll area that keeps each card at its natural height and scrolls instead of squeezing, and a
  table-only page still fills the page as before. The External tools card also moved directly
  under Appearance, so the Rclone installer is visible without scrolling.
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
