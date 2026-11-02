# 0007. Copy semantics and non-destructive defaults

- Status: Accepted
- Date: 2025-11-02

## Context

Backup software that deletes data is worse than no backup software. Users will run Nesva against
their only copy of important data, and synchronization tools that mirror deletions have caused
well-known data loss.

## Decision

- **Archive jobs are copy-only.** Sources are never deleted, moved or rewritten. The schema
  constrains `backup_jobs.copy_semantics` to `'copy'`.
- **Remote deletions are off by default** (`allow_remote_deletions DEFAULT 0`) and require an
  explicit per-job setting plus a confirmation prompt before the first destructive run.
- **No destructive operation is implicit**: retention pruning, remote deletes and restore
  overwrites each require an explicit policy and confirmation.
- **Restore never overwrites silently**: the conflict policy (`ask`/`skip`/`overwrite`/`rename`)
  is mandatory and defaults to `ask`.
- **Recursion protection is mandatory**: the destination directory, cloud sync folders and any
  path inside the source set are excluded automatically; symlink/reparse-point loops are
  detected.
- **Consistency is never over-claimed.** Without VSS, files modified during a copy are documented
  as such; sizes are never equated with cryptographic integrity, and hashes are recorded when
  available.
- **Bounded resources**: no local staging of full datasets, configurable cache location, and a
  free-space check before operations that need temporary space.

## Consequences

- Archive mode cannot "clean up" the destination automatically; users who want a mirror must ask
  for it explicitly (and Phase 2 will require confirmation).
- Documentation and UI copy must state the limitation about point-in-time consistency on
  Windows.
- Destructive code paths get dedicated tests (dry-run, cancellation, resumed runs) before they
  ship in Phase 2.

## Alternatives considered

- **Mirror-by-default (rsync-like)**: rejected - one misconfigured source path can destroy the
  remote copy.
- **Treating archive mode as a backup**: rejected - it has no versions and no integrity
  guarantees, and conflating the two would mislead users about recovery options.
