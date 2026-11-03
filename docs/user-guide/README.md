# Nesva user guide

This guide documents what the current build actually does. The full user documentation ships with
the packaged installation in Phase 6; nothing here claims a capability the software does not have.

Start here:

- [Tutorial: from install to a verified archive](tutorial.md) - the guided walkthrough, including
  the acceptance scenario to run after any change.
- [Archiving Microsoft 365 mail](microsoft-365.md) - app registration, sign-in, sync, troubleshooting.

## What you can do today (version 0.5.0)

1. Start the application: `nesva` (`--gui-selftest <dir>` renders every page headlessly and
   exits, which is how the UI is verified in CI).
2. Browse the eight pages from the sidebar (Ctrl+1 to Ctrl+8): Dashboard, Jobs, Storage, Archive,
   Snapshots, Restore, Activity, Settings.
3. Create **archive** jobs (copy files, destination stays browsable) and **backup** jobs (Restic
   snapshots, encrypted and versioned), on a manual or scheduled trigger, with exclusions,
   dry runs and a full run history.
4. Add local and Rclone storage providers and test them from the Storage page.
5. Browse a repository's snapshots, list the files inside a snapshot, restore a snapshot or a
   single file, preview and apply a retention policy, check and unlock a repository.
6. Archive a Microsoft 365 mailbox to `.eml` files with incremental delta sync
   (`nesva-cli mail`, see [the mail guide](microsoft-365.md)).
7. Let `nesva-agent` run schedules in the background, with missed-slot catch-up, retry backoff and
   a single running job per job id.
8. Install Rclone from inside Nesva, from a preview of every step (download URL, SHA-256 check,
   unpack command, target path and, for an all-users install, the exact elevated command):
   Settings -> External tools -> Install Rclone. The CLI equivalent is
   `nesva-cli tools rclone` (preview) and `nesva-cli tools rclone --install`.
9. Count the mail before copying it (`nesva-cli mail estimate <account>`) and watch a sync report
   its percentage, rate and remaining time (`nesva-cli mail sync <account> --to <dir>`).
10. Use `nesva-cli` for every one of those operations, with `--json` output for scripting.

Not yet available: the desktop UI for Microsoft 365 accounts and mail schedules, the Windows
build (unbuilt and unverified), and packaged installers. Those are Phase 5 (UI) and Phase 6.

## Where your data lives

- Linux: `$XDG_DATA_HOME/nesva` (default `~/.local/share/nesva`): `nesva.db` and `logs/`.
- Windows: `%LOCALAPPDATA%\Nesva`.
- Nothing is written next to the executable or into a system directory.

## Safety you can rely on today

- No source file is ever deleted, moved or modified. Archive jobs copy; restore jobs write into a
  directory you choose and ask before overwriting.
- Destructive actions (retention `forget --prune`, restore over existing files) are confirmed,
  and retention is previewed before it is applied.
- Credentials go to the OS keyring and to `0600` temporary files that are removed immediately;
  they never appear in the database, the configuration, the logs or a process argument list.
- Logs are structured JSON and are redacted before they are written: tokens, passwords, bearer
  headers, private keys and connection strings are censored.
- The only network traffic is the Microsoft Graph and identity requests you trigger by connecting
  a Microsoft 365 account, plus the Rclone/Restic processes Nesva starts for you.
