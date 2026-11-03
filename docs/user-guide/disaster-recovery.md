# Disaster recovery: getting your data back on a new machine

The scenario this document covers: the computer is gone, reinstalled, or will not boot. You
have a new machine and the cloud account the backups went to.

## What you need before you need it

| Item | Where it lives | What happens if it is lost |
|---|---|---|
| **Restic repository password** | OS keyring (Windows Credential Manager / Linux Secret Service), and **nowhere else** | Without it the snapshots are unrecoverable. Write it down and keep it somewhere other than the machine being backed up. |
| Restic repository location | The backup job ("Repository" field), e.g. `D:\Nesva\restic` or `rclone:icloud:Nesva/restic` | Recoverable: list the remote and look for `config`, `data/`, `index/`, `snapshots/`. |
| Rclone remote configuration | `rclone config` (its own config file, `%APPDATA%\rclone` on Windows) | Recreate the remote: same provider, same account, same remote name (`icloud:`). |
| Microsoft 365 app registration | Entra ID | Re-register the app (see the Microsoft 365 guide) and sign in again. |
| Nesva itself | Installer / build | Install it; nothing else has to be restored first. |

Nesva's own database is **not** needed to read the data:

* archived files are plain files at the destination, readable without Nesva;
* Restic snapshots are self-contained in the repository.

## Restoring without Nesva at all

Archive mode wrote ordinary files, so browsing the destination is enough. For Restic:

```
restic -r <repository> snapshots                 # what exists
restic -r <repository> ls latest                 # what is inside
restic -r <repository> restore <snapshot> --target C:\Restore
```

Restic asks for the repository password. This works on any machine with the restic binary,
with or without Nesva.

## Restoring with Nesva

1. Start Nesva on the new machine. It creates an empty database; nothing else is needed.
2. **Storage** -> **Configure Rclone...** (rclone config) and create the same remote name.
3. **Backup Jobs** -> **New job** -> mode **Backup**, Repository = the same location.
4. **Set password...** and enter the password you wrote down for that repository.
5. **Snapshots** -> **Refresh from repository** -> pick a snapshot -> **Restore snapshot...**.

For the mail archive, do the reverse of the original setup: connect the account again
(`nesva-cli mail connect` or the Mail page) and point the mail job at a directory; the
delta cursor starts fresh, so the first run downloads what is missing and skips what is
already on disk.

## What is not recoverable

* **A lost repository password.** There is no recovery, a back door or a reset. That is what
  encryption means; keep the password somewhere else.
* **Run history and job definitions** unless the database itself was backed up. The data is
  safe; only the convenience metadata is gone.
* **Mail that was never archived** (a message deleted from the mailbox before a sync).

## Before you rely on it

Test the recovery once, on a scratch machine or a scratch folder: restore a snapshot,
open a file, then delete the copy. An untested backup is an assumption.
