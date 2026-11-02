# 0012. Restic integration, credential handling and restore staging

- Status: Accepted
- Date: 2026-11-02

## Context

Phase 4 adds versioned, optionally encrypted backups through Restic, with repository
initialisation, backup, snapshot browsing, restore, retention, verification and lock detection.
Restic needs a repository password; Nesva must never store it in plaintext or leak it into a
process list or a log.

## Decision

**Restic is driven, never reimplemented**

- `nesva::backup::ResticRepository` runs the user's `restic` binary through an injected
  `ResticRunner` (production: `QProcess`, argument arrays, separate channels, kill on cancel).
- Argument construction is a pure function (`command::*`) and is unit-tested: `--repo`,
  `--password-file`, `--json`, `--exclude`/`--iexclude`, `--tag nesva`, `--limit-upload`.
- Restic's documented exit codes are mapped precisely: 10 → `NotFound`, 11 → `AlreadyExists`
  ("repository is locked"), 12 → `PermissionDenied` ("incorrect password"), everything else →
  `ExternalToolFailed` with the trimmed stderr.

**Password handling**

- The password is written to a **temporary file with 0600 permissions** in the Nesva cache
  directory and passed as `--password-file`; it is removed on every exit path. It never appears on
  the command line, in the database, in settings or in logs.
- Preferred storage is the OS credential store through `security::SecretStore`:
  - Linux: freedesktop.org Secret Service (`org.freedesktop.secrets`) over D-Bus — works with
    GNOME Keyring, KWallet's `ksecretd` and compatible providers. The D-Bus marshalling builds
    `a{ss}` attributes and the `a{sv}` item properties explicitly, because Qt would otherwise send
    `a{sv}` for both and the service rejects it.
  - Windows: Credential Manager (`CredWriteW`/`CredReadW`, per-user, DPAPI protected).
- A password file is supported as an explicit fallback (`PasswordSource::File`) for headless
  setups; the file is the user's, Nesva only reads it.
- The keyring account is derived from the repository location (`restic-<sha1 prefix>`), so moving
  a job to another repository never silently reuses the old secret.

**Restore never overwrites**

- `restic restore` always writes over existing files, which is exactly what Nesva must not do.
  Restore therefore stages into `<destination>/.nesva-restore-XXXXXX`, then **moves** each file
  into place according to the conflict policy (`skip`, `rename`, `overwrite`; `ask` is refused
  before anything runs), stripping the snapshot's common root so files land directly in the chosen
  folder. Nothing is copied twice; the staging directory is removed afterwards.

**Retention**

- `restic forget --json` is parsed per group: `keep` and `remove` (which is `null` when nothing
  is removed). Preview uses `--dry-run`; applying without `--prune` never deletes data packs, and
  `--prune` is an explicit opt-in. An empty policy is rejected instead of interpreted as "delete
  everything".

**Verification and locks**

- `restic check --json` is parsed for `message_type: error` and the summary's `num_errors`.
- `restic list locks` surfaces stale locks; `restic unlock` is available with a confirmation.

## Consequences

- Restic remains a user-installed tool; Nesva detects it, never bundles or downloads it.
- Repository access is exclusive where Restic requires it: a locked repository yields a clear
  "locked by PID ... on host ..." error instead of a hang.
- The tests run against **real repositories**: `tst_restic` covers init, backup, incremental
  behaviour, snapshot listing, `ls`, restore, `check --read-data 100%`, retention dry-run and
  execution, and the documented exit codes; `tst_backup_runner` covers the job-level path
  including cancellation, dry-run and conflict policies.
- Windows-specific paths (Credential Manager, task registration) remain unverified until the
  Windows test round.

## Alternatives considered

- **A Restic-compatible repository implementation**: forbidden — never implement custom
  cryptography or a custom backup format.
- **Passing the password via `RESTIC_PASSWORD`**: visible in `/proc/<pid>/environ` to processes
  of the same user and easily captured in diagnostics; a 0600 file is narrower.
- **Restoring directly over the destination**: silently overwrites user files — unacceptable.
- **Copying the staged tree instead of moving it**: doubles peak disk usage.
