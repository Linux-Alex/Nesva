# Tutorial: from install to a verified archive

This walks through the whole product in the order a new user meets it: check the environment,
install Rclone from inside Nesva, archive a folder, take a versioned backup, restore both, and
archive a Microsoft 365 mailbox. Every command below was executed on Linux while writing this
document; the expected output is shown where it matters.

Two words are used carefully throughout:

- **Archive** copies files so they stay usable at the destination. It is a copy, not a version
  history.
- **Backup** creates Restic snapshots: versioned, deduplicated and encrypted. That is the one
  that protects you against an accidental deletion or a ransomware encryption.

An archive is never a substitute for a backup, and Nesva never presents it as one.

---

## 0. What you need

- Linux (tested) or Windows (builds, not yet verified end to end).
- A build of Nesva: see [building](../developer-guide/building.md).
- Somewhere to archive to: a local disk, a mounted network share, or a cloud remote.
- For backup mode, a place to keep a Restic repository (it can be the same disk).

---

## 1. Check the environment

```bash
./build/linux-debug/nesva-cli status
./build/linux-debug/nesva-cli doctor
./build/linux-debug/nesva-cli tools
```

`status` shows the data directory, the database schema version and the job counters. `doctor`
checks the database, the log directory, free space and the external tools. `tools` lists the
programs Nesva calls:

```
TOOL    PATH                           VERSION
------  -----------------------------  ---------------------------------------------------
rclone  -                              not installed (nesva-cli tools rclone --install)
restic  /home/aleks/.local/bin/restic  restic 0.18.1 compiled with go1.25.1 on linux/amd64
```

---

## 2. Install Rclone from Nesva

Nesva does not bundle Rclone and will not pipe a download into a shell. Instead it shows you
exactly what it would do, and then does it.

**Desktop:** Settings -> *External tools* -> **Install Rclone...**
**CLI:** preview first, then install.

```bash
nesva-cli tools rclone              # preview only, nothing is downloaded
```

```
Installation plan - nothing has been downloaded, written or executed yet:
  Rclone 1.75.2 -> /home/aleks/.local/bin/rclone (user scope)
  1. Download rclone 1.75.2 to a private temporary directory
       https://downloads.rclone.org/v1.75.2/rclone-v1.75.2-linux-amd64.zip
  2. Compute its SHA-256 and compare it with the published checksum
       https://downloads.rclone.org/v1.75.2/SHA256SUMS
  3. Unpack the archive
       $ unzip -o <archive> -d <temporary directory>
  4. Install the binary to /home/aleks/.local/bin/rclone
       A plain file copy inside your own home directory: no password and no elevated privileges.
  5. Run the installed binary and record its path
       $ /home/aleks/.local/bin/rclone version
```

What the plan guarantees:

- The archive is **verified by SHA-256** against `SHA256SUMS` before anything is unpacked, and
  nothing is installed if the hash does not match.
- The download and the unpacking happen in a private temporary directory; only the single
  verified binary is copied into place.
- There is **no shell**: every step is a program with an argument vector, shown verbatim.
- The default scope is per-user, which needs no privileges at all. An all-users install shows
  the elevated command (`pkexec install -m 0755 ...`) *before* you approve it, and the archive is
  never unpacked as root.

```bash
nesva-cli tools rclone --install
```

```
  Downloading rclone-v1.75.2-linux-amd64.zip
  Verifying the SHA-256 checksum
  Unpacking the archive
  Checking the installed binary
Installed rclone 1.75.2 at /home/aleks/.local/bin/rclone (SHA-256 verified).
Nesva now uses that path, so it works without changing PATH.
```

`nesva-cli doctor` now reports `rclone: /home/aleks/.local/bin/rclone (rclone v1.75.2)`.

---

## 3. Archive a folder (copy semantics)

Suppose you keep documents in `~/Documents` and want a second copy on an external disk mounted
at `/media/backup`.

**Desktop:** Backup Jobs -> **New job**

1. Name: `Documents to external disk`. Mode: **Archive** (copy).
2. Source: `~/Documents`. Destination: `/media/backup/Nesva/documents`.
3. Leave exclusions empty for the first run, tick *Verify checksums*, then **Save and run**.

**CLI equivalent:**

```bash
nesva-cli jobs list
nesva-cli jobs run 1 --dry-run    # shows what would be copied, writes nothing
nesva-cli jobs run 1
```

A dry run is the safest first contact with a new source: it reports every file that would be
copied and every file that would be skipped, with the reason, and touches nothing.

What matters about an archive run:

- Source files are **only read**. Nothing on the source side is deleted, moved or modified.
- Files already transferred and unchanged are skipped by size and modification time, so later
  runs are fast.
- Every run records a manifest, so the Archive Explorer can show what is where, with hashes.
- A cancellation leaves a consistent state: the file in flight is completed, nothing else.

---

## 4. Prove the archive is readable

Do not trust a backup you have never restored. Restore one file:

**Desktop:** Restore -> pick the job -> choose a file -> restore into `~/RestoreTest`.
**CLI:**

```bash
nesva-cli archive list 1 --limit 10
nesva-cli archive restore 1 --to ~/RestoreTest --policy skip
```

`--policy` is `skip`, `rename` or `overwrite`; nothing is overwritten unless you ask for it.
Checksums are verified after the copy.

---

## 5. A versioned backup with Restic

Archive gives you a copy. A backup gives you history: every run is a snapshot you can go back
to, encrypted with a password you keep.

```bash
# 1. Store the repository password in the OS keyring (never in a file or a command line)
nesva-cli repository init 2 --password-stdin
# 2. First snapshot
nesva-cli jobs run 2
# 3. See what exists
nesva-cli snapshots list 2
```

In the desktop application the same thing lives in the job editor (mode **Backup**, repository
path, retention policy, *Set password*, *Initialise repository*) and on the Snapshots page
(Refresh, Browse files, Restore, Retention preview/apply, Check, Unlock).

Retention is previewed before it is applied, and `--prune` frees the space:

```bash
nesva-cli retention preview 2
nesva-cli retention apply 2 --prune
```

Restoring from a snapshot, including an older version of a file:

```bash
nesva-cli backup restore 2 --snapshot latest --to ~/RestoreTest --policy rename
```

---

## 6. Archive a Microsoft 365 mailbox

The full setup (a user-scoped app registration, no client secret, no admin required) is in
[microsoft-365.md](microsoft-365.md). Once the app registration exists:

```bash
nesva-cli mail connect --account you@example.com \
    --tenant <directory-id> --client <application-client-id>
```

Then **count before you copy**, so the size of the job is known:

```bash
nesva-cli mail estimate 1
```

```
FOLDER                                    IN MICROSOFT  ARCHIVED  REMAINING
----------------------------------------  ------------  --------  ---------
Arhiv                                     46            46        0
Prejeto                                   2892          469       2423
Prejeto/ARC Tickets                       1964          0         1964
...
6652 messages in 22 enabled folder(s); 1771 archived, 4881 to go.
```

Then sync, with live progress:

```bash
nesva-cli mail sync 1 --to ~/Archive/Mail
```

```
[ 42.3%] 2053/4881 messages · 7.9 msg/s · ETA 6m 0s · 1.2 GB archived · Prejeto
```

- The percentage is against the folder counters Microsoft reports, so it means "of the mail in
  scope". An incremental run (only changes since the last run) says `[delta]` instead, because a
  percentage of the whole mailbox would be misleading.
- The rate and the ETA come from the run itself: messages examined divided by wall time.
- `mail estimate` uses the measured speed of the previous sync, so a later estimate is closer
  than a guess.
- Every message becomes `<folder>/<YYYY>/<MM>/<subject>-<id12>.eml`, written atomically, with
  its SHA-256 recorded. Nothing is ever deleted from the mailbox.
- `Ctrl-C` is safe: the delta cursor only moves past messages that were actually handled.

If a run is interrupted or bounded with `--max-messages`, the next run continues from the same
place. To re-walk everything (for example after moving the archive to a new disk):

```bash
nesva-cli mail sync 1 --to ~/Archive/Mail --full
```

Already-archived messages are still skipped by timestamp, so `--full` only downloads what is
missing.

Verify a file by opening one in a mail client, or check the headers:

```bash
head -5 ~/Archive/Mail/Prejeto/2026/11/*.eml
```

---

## 7. Automate it

The scheduler lives in `nesva-agent`, so jobs run with the window closed and after a reboot.

```bash
nesva-cli schedule list
nesva-agent                       # foreground, tick every 20 s
scripts/install-agent-service.sh  # systemd user unit, starts at login
```

Every schedule has a live next-run preview in the desktop schedule editor. Missed slots are
caught up once, failed runs are retried with a persisted backoff, and the database guarantees a
job cannot run twice at the same time - whichever component starts it.

---

## 8. Acceptance scenario (what to check after a change)

Work through this list on a fresh data directory (`--data-dir /tmp/nesva-test`) before trusting a
build with real data.

| # | Step | Expected result |
|---|---|---|
| 1 | `nesva-cli doctor` | every check `OK`, schema version equals the latest |
| 2 | `nesva-cli tools rclone` | a plan with a resolved version and no changes made |
| 3 | `nesva-cli tools rclone --install` | checksum verified, binary runs, path recorded |
| 4 | Create an archive job, run with `--dry-run` | list of files, zero writes |
| 5 | Run the job for real | objects in the destination, manifest rows, exit code 0 |
| 6 | Delete one archived file, run again | the file is copied again (self-healing) |
| 7 | `nesva-cli archive restore ... --policy rename` | restored copy exists, hash verified |
| 8 | Create a backup job, `repository init`, run | snapshot listed by `snapshots list` |
| 9 | `retention preview` | the exact set that would be removed, nothing removed yet |
| 10 | `repository check` | repository integrity reported OK |
| 11 | `mail estimate <id>` | per-folder counts, a total, and a duration once a sync has run |
| 12 | `mail sync` twice | the second run reports `[delta]` and downloads nothing |
| 13 | Interrupt a sync with Ctrl-C, run again | continues from the same place, no duplicates |
| 14 | Start `nesva-agent`, wait for a scheduled slot | the run happens with no window open |
| 15 | Look at `~/.local/share/nesva/logs/nesva.log` | JSON events, no secrets, no message bodies |

---

## 9. Where things are

| What | Where |
|---|---|
| Database | `~/.local/share/nesva/nesva.db` |
| Logs | `~/.local/share/nesva/logs/nesva.log` (JSON, redacted, rotated) |
| Restic passwords | OS keyring (Linux: Secret Service; Windows: Credential Manager) |
| Microsoft 365 refresh token | OS keyring, `nesva/msgraph/<tenant>/<upn>` |
| Archived mail | the `--to` directory you chose |

If something goes wrong, `nesva-cli status --json` and the tail of the log file usually say
exactly what happened; errors carry a stable code and never contain credentials.
