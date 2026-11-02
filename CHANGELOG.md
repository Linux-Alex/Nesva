# Changelog

All notable changes to Nesva are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
