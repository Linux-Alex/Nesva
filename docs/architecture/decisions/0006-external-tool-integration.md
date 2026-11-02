# 0006. Rclone and Restic as external tools invoked without a shell

- Status: Accepted
- Date: 2025-11-02

## Context

Nesva must talk to many cloud providers and must produce versioned, encrypted backups. Writing
provider clients and a backup engine from scratch would be years of work and a security
liability. Rclone and Restic are mature, licensed tools that cover both needs.

## Decision

- Rclone provides remote transfers (SFTP, S3-compatible, WebDAV, iCloud Drive, and the rest of
  its remote catalogue). Restic provides encrypted, versioned snapshots.
- Both are invoked through `QProcess` with an **argument array**. No shell, no string
  concatenation, no `system()`, no `popen()`. Paths with spaces and Unicode therefore need no
  quoting and cannot inject metacharacters (`tst_external_tool` proves this).
- `stdout` and `stderr` are captured separately, exit codes are inspected, timeouts bounded, and
  cancellation supported.
- Machine-readable output (JSON where available, e.g. `rclone --use-json-log`) is preferred over
  parsing human-readable progress text.
- Tools are **detected, not bundled** in this phase: absolute path from settings
  (`tools.rclone_path` / `tools.restic_path`) or resolution on `PATH`, then a version probe.
  Missing tools degrade the feature set with an explicit warning (`doctor`, dashboard), never a
  silent failure.
- Verification of bundled binaries (if ever bundled) requires pinned versions and checksums;
  downloading binaries at runtime is prohibited.
- Licences are reviewed in Phase 6 before any redistribution; Rclone (MIT) and Restic (BSD-2)
  are compatible with Apache-2.0 redistribution.

## Consequences

- Users must install two well-documented tools; `doctor` tells them exactly what is missing.
- Nesva stays free of provider-specific API clients (notably no custom iCloud client), which
  keeps the attack surface small.
- Version differences are handled by minimum-version checks and explicit error messages.
- Process invocation, cancellation and progress parsing become first-class testing targets.

## Alternatives considered

- **Native provider SDKs (AWS SDK, Azure SDK, WebDAV libraries)**: huge dependency surface and
  inconsistent iCloud support.
- **A custom Restic-compatible repository format**: explicitly forbidden - never implement custom
  cryptography or a custom backup format.
- **Bundling binaries in the installer now**: postponed to Phase 6 with a documented verification
  policy.
