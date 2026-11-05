# Nesva

Nesva is a cross-platform desktop application for **archiving** normal files to remote storage
and for creating **versioned, optionally encrypted backups** with Restic.

It is written in C++20 with Qt 6 Widgets, built with CMake + Ninja, and stores its own state in
SQLite. Rclone and Restic are used as external, user-supplied tools - never bundled silently,
never invoked through a shell.

- Licence: Apache-2.0 (NESVA's own code, subject to dependency-licence review)
- Executables: `nesva` (desktop), `nesva-cli` (automation), `nesva-agent` (scheduler/runner)

---

## Download

Current release: [v0.6.3](https://github.com/Linux-Alex/Nesva/releases/latest)

| Platform | File | Notes |
| --- | --- | --- |
| Windows x86_64 | `nesva-<version>-windows-x86_64-setup.exe` | Installer: per-user install in `%LOCALAPPDATA%\Programs\Nesva`, shortcuts, uninstaller - **no administrator rights** |
| Windows x86_64 | `nesva-<version>-windows-x86_64.zip` | Portable: unpack anywhere, run `nesva.exe`; Qt runtime included |
| Linux x86_64 | `nesva_<version>_amd64.deb` | `sudo apt install ./nesva_<version>_amd64.deb`; recommends `rclone` and `restic` |
| Linux x86_64 | `nesva-<version>-linux-x86_64.tar.gz` | Portable tree with Qt bundled: unpack, run `nesva-<version>-linux-x86_64/bin/nesva` |

Each release also publishes a `SHA256SUMS` file per platform. Rclone and Restic are not bundled:
Nesva installs them from inside the application (**Settings -> External tools**), showing every
step and verifying the published SHA-256 before anything runs.

The binaries are not code-signed, so Windows SmartScreen warns on first launch. The Windows agent
runs from the logon Run entry - no PowerShell, no scheduled task, no elevation.

---

## Archive mode vs. backup mode

| | Archive mode | Backup mode |
|---|---|---|
| Purpose | Keep files usable in the storage provider | Versioned, immutable snapshots |
| Engine | Direct provider / Rclone transfers | Restic repositories |
| Semantics | **Copy only** - sources are never deleted | Snapshots with retention policy |
| Recovery | Files are browsable in the provider itself | Snapshot restore through Restic |
| Encryption | Provider-defined | Restic encryption |

Archive mode is never presented as an equivalent to a versioned, immutable backup.

---

## Current status: v0.6.3 - Phases 1-5 implemented

Everything below is implemented, built and tested on Linux **and Windows**: the Windows build is
verified on a real machine (24/24 test binaries, GUI self-test, background agent and autostart),
Linux is verified locally and on GitHub Actions. Phase 5's Microsoft 365 engine, CLI and mail page
are in place. Archive jobs run end to end, scheduled jobs run in a background agent without the
window, versioned encrypted backups work from the desktop application or `nesva-cli`, and
Microsoft 365 mail can be exported as incremental .eml archives from the CLI. What exists today
is real, working functionality - not mockups:

**nesva-core (library, no Qt Widgets dependency)**
- `Result<T>` / `Status` error model with stable, machine-readable error codes
- Structured logging: newline-delimited JSON file sink with rotation, human console sink,
  per-sink minimum levels, and mandatory credential redaction
- Credential redaction: registered secrets, `key=value` assignments, Bearer/Basic headers,
  PEM private-key blocks, AWS key ids, GitHub tokens, JWT-shaped tokens, recursive JSON redaction
- Per-user path resolution (XDG on Linux, `%LOCALAPPDATA%` on Windows) with override support
- External tool probing without a shell (path resolution, version capture, exit-code handling)
- SQLite layer: WAL journal, foreign keys enforced on every connection, busy timeout,
  prepared statements, RAII transactions with nested savepoints, integrity checks
- Versioned schema migrations (schema v1: 17 tables covering settings, storage, jobs, runs,
  events, manifests, snapshots, restores and Microsoft 365 mail)
- Typed settings store (`string`/`integer`/`boolean`, defaults, no silent rewrites)

**Archive engine (Phase 2, in nesva-core)**
- File scanner with glob/regex/literal exclusions, symlink and depth protection, destination
  exclusion and an explicit skip reason for everything that is not archived
- Storage providers with a capability model: local filesystem (atomic writes, size and SHA-256
  verification, mtime preservation) and Rclone (`copyto`/`lsjson`/`hashsum`, argument arrays,
  retries, cancellation)
- Archive runner: incremental runs from the previous manifest, per-file manifest and
  archive-object records, job events, counters, cancellation, dry-run that writes nothing
- Restore with conflict policies (skip/rename/overwrite), post-restore hash verification, and a
  recorded `restore_jobs` row per attempt
- Job store with database-enforced single flight (one running run per job) and crash recovery of
  interrupted runs

**Background agent (Phase 3)**
- `nesva-agent` schedules and executes jobs without the desktop window: daily/weekly/monthly,
  on-login and on-startup triggers, timezone-aware next-run computation, missed-run catch-up,
  persistent retries with backoff, maximum run duration, graceful shutdown
- Local JSON socket for `status`/`run`/`cancel`/`reload`/`shutdown`, plus a heartbeat file so the
  UI always knows the agent state; single-instance lock per data directory
- Duplicate runs remain impossible thanks to the database guard, whichever component starts them
- systemd **user** unit and a per-user Windows scheduled task (never SYSTEM)

**Restic backups (Phase 4, engine + CLI)**
- Repository initialisation, backup with live progress, snapshot listing and file browsing,
  restore, retention preview/apply (optional prune), integrity check (`--read-data-subset`), lock
  listing and unlock
- Repository passwords live in the OS keyring (freedesktop Secret Service on Linux, Windows
  Credential Manager) and reach restic through a 0600 temporary file - never a command line,
  the database or a log
- Restore stages the snapshot and then moves files into place with skip/rename/overwrite
  policies; existing files are never overwritten silently
- Desktop: backup jobs in the job editor (repository, retention, password, repository
  initialisation), a Snapshots page with repository refresh, restore, retention preview/apply,
  integrity check and unlock, and a snapshot restore dialog with progress

**Microsoft 365 mail (Phase 5, engine + CLI)**
- OAuth 2.0 authorization code flow with PKCE for a public desktop client: loopback redirect,
  CSRF state check, no client secret anywhere
- Refresh token in the OS keyring, access token in memory only; revocation is immediate
- Read-only Microsoft Graph usage (`/me`, folders, per-folder delta feed, MIME endpoint) with
  throttling (429 `Retry-After`), 5xx backoff and one 401 refresh
- Incremental EML export: per-folder delta cursor in SQLite, restartable mid-run, unchanged
  messages skipped, removed messages keep their local file
- CLI: `mail connect`, `mail accounts`, `mail folders`, `mail estimate`, `mail sync`,
  `mail status`
- Progress you can act on: `mail estimate` counts every folder from Microsoft's own counters and
  turns the measured speed of the previous run into a duration; `mail sync` reports
  `[ 42.3%] 2053/4881 messages · 7.9 msg/s · ETA 6m 0s` and switches to `[delta]` for a run that
  only carries changes, where a percentage of the whole mailbox would be misleading

**External tools (Rclone)**
- One-click installation that never pipes a download into a shell: Nesva resolves the current
  release, shows every step before anything happens - the download URL, the published SHA-256,
  the unpack command, the target path - and then performs exactly those steps
- The archive is verified against the publisher's `SHA256SUMS` before unpacking; a mismatch
  installs nothing
- Per-user install by default (no privileges, no password). An all-users install shows the exact
  elevated command (`pkexec install -m 0755 ...`) before the password prompt; the archive is
  never unpacked as root
- After installation Nesva records the absolute path, so Rclone works even when
  `~/.local/bin` is not on `PATH`
- Desktop: Settings -> External tools -> Install Rclone / Install Restic; CLI:
  `nesva-cli tools rclone|restic`
  (`--install`, `--system`)

**nesva (desktop application)**
- Qt 6 Widgets shell: sidebar navigation, top status bar, page stack, bottom activity dock
- Eight pages: Dashboard, Backup Jobs, Storage, Archive Explorer, Snapshots, Restore,
  Activity, Settings
- Dashboard: job counters, last success/failure, next scheduled run, storage count,
  environment warnings (Rclone/Restic detection), recent activity
- Backup Jobs page: create, edit, delete, run, cancel and inspect history; the run executes
  on a worker thread with live progress while the window stays responsive
- Job editor with source picker, local-folder or storage-provider destination, exclusions,
  bandwidth limit, checksum verification and a dry-run preview
- Archive Explorer (destination listing joined with manifest hashes) and a Restore wizard
- Storage connection manager: local folders and Rclone remotes, with an explicit connection test
- Schedule editor with a live next-run preview; "Run now" is delegated to the background agent
  when it is running, and the status bar shows agent state
- Optional start-at-sign-in in the notification area (`nesva.exe --tray`), minimise-to-tray
  and desktop notifications for finished runs
- Backup Jobs and Storage pages read the real database through Qt Model/View models
- Activity page: combined job + application event stream with live text filtering, sortable
  columns, and a shortcut to the log folder
- Settings page: theme (system/light/dark), log verbosity, destructive-action confirmation,
  real path inspection, and an External tools card that reports Rclone/Restic and offers the
  Rclone installer with its full step preview - all persisted in SQLite
- Light and dark palettes, programmatic theme-aware icon set, high-DPI, keyboard navigation
  (Ctrl+1..Ctrl+8), native menu bar, system tray integration
- `--gui-selftest` mode that drives every page and writes PNG screenshots (headless or X11)

**nesva-cli (automation and diagnostics)**
- `status`, `doctor`, `tools` (`tools rclone|restic [--install] [--system]`),
  `jobs list`, `jobs run` (`--dry-run`, `--force`, `--retries`),
  `jobs cancel`, `storage list`, `storage test`, `archive list`, `archive restore`,
  `history list`, `verify`, `schedule list`, `agent status|run|cancel|reload|stop`,
  `mail connect|accounts|folders|estimate|sync|status`,
  `repository init|check|locks|unlock|set-password`, `snapshots list`,
  `retention preview|apply` and `backup restore` implemented against the real database,
  repositories and agent socket, with `--json` output and documented exit codes
- Commands that belong to later phases return exit code 4 with an explicit
  "planned for Phase N; nothing was changed" message

Pages and commands that belong to later phases are labelled as such in the UI and in the CLI.
Nothing pretends to have succeeded.

---

## Requirements

Verified development environment (Ubuntu 26.04, x86_64):

| Component | Requirement | Verified version |
|---|---|---|
| C++ compiler | C++20 (GCC 12+/Clang 15+/MSVC 19.30+) | GCC 15.2.0 |
| CMake | 3.25+ | 4.2.3 |
| Ninja | 1.10+ | 1.13.2 |
| Qt | 6.5+ (Core, Gui, Widgets, Sql, Network, Concurrent, Test) | 6.10.2 |
| SQLite | via Qt SQL SQLite plugin | 3.46.1 |
| Rclone | optional at build time; required for cloud remotes | 1.75.2, installed by Nesva |
| Restic | optional at build time; required for backup mode | 0.18.1 (SHA-256 verified install) |

On Debian/Ubuntu the Qt dependencies are:

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-base-dev-tools \
                 libqt6sql6-sqlite libgl1-mesa-dev
```

---

## Build

Every command below was executed in this repository on Linux:

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug --output-on-failure
```

Release build:

```bash
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release --output-on-failure
```

Other presets: `linux-clang-debug` (requires clang), `linux-asan` (ASan + UBSan),
`windows-msvc-debug` / `windows-msvc-release` (run from a Visual Studio developer prompt).

See `docs/developer-guide/building.md` for prerequisites per platform, troubleshooting and
packaging.

---

## Run

```bash
./build/linux-debug/nesva                # desktop application
./build/linux-debug/nesva-cli status     # CLI diagnostics
./build/linux-debug/nesva-cli doctor     # environment and dependency checks
```

Useful options:

```bash
./build/linux-debug/nesva --data-dir=/tmp/nesva-dev --help
./build/linux-debug/nesva --gui-selftest=/tmp/nesva-shots --no-console-log
./build/linux-debug/nesva-cli --json status
```

Application data (database, logs, cache) lives in the per-user data directory:

- Linux: `$XDG_DATA_HOME/nesva` (default `~/.local/share/nesva`)
- Windows: `%LOCALAPPDATA%\Nesva`

Override order: `--data-dir` → `NESVA_DATA_DIR` → platform default. Nothing is ever written
next to the executable or into Program Files.

---

## Testing

`ctest --preset linux-debug` runs 22 test binaries (238 test functions):

| Test | Covers |
|---|---|
| `tst_result` | `Result`/`Status` semantics, error codes, display strings |
| `tst_redactor` | Secret masking: assignments, bearer/JWT/AWS/PEM shapes, nested JSON |
| `tst_logging` | JSON log lines, level filtering, rotation, redaction, reopen after deletion |
| `tst_paths` | Data-root resolution order, derived paths, directory creation failures |
| `tst_database` | Migrations, idempotency, FK enforcement, WAL, transactions, savepoints, integrity |
| `tst_settings_store` | Typed get/set, defaults, persistence, unicode, key validation |
| `tst_external_tool` | Path resolution, version probing, non-zero exit, no shell interpretation |
| `tst_exclusions` | Glob/regex/literal rules, anchoring, invalid patterns |
| `tst_file_scanner` | Scanning, exclusions, symlinks, depth limit, cancellation, unicode |
| `tst_job_store` | Job CRUD, source labels, single-flight, stale-run recovery, manifests |
| `tst_local_provider` | Atomic upload/download, verification, traversal rejection, unicode |
| `tst_rclone_provider` | Exact argv, no shell, JSON parsing, retries, cancellation, error mapping |
| `tst_archive_runner` | End-to-end archive, incremental rerun, dry run, recursion guard, failures |
| `tst_archive_restore` | Conflict policies, verification, corruption detection, restore records |
| `tst_schedule` | Next-run computation, DST, missed runs, retries, due decisions |
| `tst_agent_protocol` | Request/status JSON, heartbeat round-trip, socket naming |
| `tst_secret_store` | Credential store round-trip against the real Secret Service |
| `tst_restic` | Restic argv/parsers plus real repository lifecycle, retention, check, restore |
| `tst_backup_runner` | Job-level backup, incremental runs, cancellation, dry-run, conflict policies |
| `tst_mail` | PKCE (RFC 7636 vector), Graph parsing, EML export, delta resume, dedupe |
| `tst_agent_integration` | Starts the real `nesva-agent`, runs a scheduled job, drives IPC |
| `tst_gui_smoke` | Headless: real MainWindow, 8 pages, models, theme, worker-thread execution |

GUI verification additionally runs in two modes:

```bash
# Headless (CI-safe)
QT_QPA_PLATFORM=offscreen ./build/linux-debug/nesva --gui-selftest=/tmp/shots --no-console-log

# Real X11/Wayland session
DISPLAY=:0 ./build/linux-debug/nesva --gui-selftest=/tmp/shots --no-console-log
```

Both write one PNG per page (`gui-selftest-N-<page>.png`) and exit non-zero if any page fails
to render.

---

## Repository layout

```text
nesva/
├── CMakeLists.txt, CMakePresets.json
├── cmake/                       CMake helper modules (warnings, generated version header)
├── include/nesva/               public headers for nesva-core
├── src/
│   ├── core/                    logging, redaction, paths, errors, external tools
│   ├── backup/                  Restic repository, argument builders and JSON parsers
│   ├── security/                OS credential store (Secret Service / Credential Manager)
│   ├── desktop/                 Qt 6 Widgets application (library + main.cpp)
│   ├── database/                SQLite access, migrations, settings store
│   ├── cli/                     nesva-cli
│   ├── jobs/                    job store, archive runner, archive restore
│   ├── providers/               storage provider interface, local + Rclone providers
│   ├── scan/                    file scanner and exclusion rules
│   ├── mail/                    Microsoft 365: OAuth PKCE, Graph client, EML exporter
│   ├── tools/                   external tool installation (Rclone) with a reviewed plan
│   ├── agent/                   agent service, IPC protocol and client
│   ├── scheduler/               schedule model (next run, missed runs, retries)
│   └── security/                keyring / Credential Manager secret store
├── tests/unit/                  Qt Test suites registered with CTest
├── docs/architecture/           overview + ADRs
├── docs/developer-guide/        build, test and packaging instructions
├── docs/user-guide/             tutorial, Microsoft 365 setup, troubleshooting
├── packaging/                   desktop entry, Windows installer staging (Phase 6)
├── assets/icons/                application icon source
└── scripts/                     developer helper scripts
```

---

## Safety guarantees in this build

- **No source deletion, ever.** Archive jobs are copy-semantics only (`copy_semantics` is
  constrained to `'copy'` in the schema) and nothing in the codebase deletes, moves or rewrites a
  source file.
- **No destructive remote operations.** There is no delete, prune or mirror code path: transfers
  only copy. `allow_remote_deletions` defaults to `0` and nothing reads it yet.
- **The destination is excluded from the scan**, so a job can never archive its own output.
- **Cancellation never reports success**, and a run whose owning process died is marked
  `interrupted` at next start instead of blocking the job forever.
- **Restore never overwrites silently.** The default policy is "skip"; "overwrite" requires an
  explicit choice and a confirmation. Every restored file is verified against the manifest hash.
- **No credentials anywhere.** No credential is stored, logged or transmitted; logs are redacted
  by construction, and credential *references* (not secrets) are the only thing the schema holds.
- **Confirmation first.** The destructive-action confirmation setting defaults to enabled.
- **Installs are reviewed and verified.** The Rclone installer shows every step before running it,
  checks the publisher's SHA-256 before unpacking anything, installs per-user by default, and
  never pipes a download into a shell. An all-users install displays the exact elevated command
  first and unpacking never happens as root.

---

## Security

Report vulnerabilities privately - see `SECURITY.md`. Logs never contain tokens, passwords or
cookies: the redactor censors registered secrets plus common credential shapes before any sink
writes a record.

---

## Roadmap

| Phase | Scope | Status |
|---|---|---|
| 1 | Foundation: build system, core services, SQLite, logging, desktop shell, CLI, tests | **delivered** |
| 2 | Functional archive: job editor, local + Rclone providers, execution, progress, restore | **delivered** |
| 3 | Background execution: agent, IPC, scheduling, missed runs, tray, notifications | **delivered** |
| 4 | Restic: repository setup, snapshots, restore, retention, verification | **delivered** |
| 5 | Microsoft 365: OAuth (PKCE), mailbox selection, incremental EML export | **in progress** - engine + CLI delivered, UI next |
| 6 | Distribution: Windows installer, .deb/AppImage/RPM, CI/CD, user docs | planned |

---

## Licence

Apache-2.0. See `LICENSE`. Third-party runtime dependencies (Qt, SQLite, Rclone, Restic) keep
their own licences; the dependency policy is documented in
`docs/architecture/decisions/0006-external-tool-integration.md`.
