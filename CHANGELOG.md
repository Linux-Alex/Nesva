# Changelog

All notable changes to Nesva are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
