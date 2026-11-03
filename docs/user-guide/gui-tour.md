# GUI tour: using Nesva from the desktop application

A short guide to the eight pages and the order in which to use them. The
[full tutorial](tutorial.md) covers the same ground with CLI equivalents and an acceptance
scenario; this page is only about what you click.

Start it with `./build/linux-debug/nesva`. The window has a sidebar on the left
(Ctrl+1 ... Ctrl+9), a status bar at the bottom, and the pages in the middle. Every page body
scrolls, so a small window never hides a control.

---

## The five-minute path

1. **Settings (Ctrl+8) -> External tools -> Install Rclone...** - review the shown steps, press
   Install, wait for "Installed rclone 1.75.2 at ...". Close the dialog.
2. **Backup Jobs (Ctrl+2) -> New job...** - Mode **Archive**, pick a folder, pick a destination
   folder, tick *Verify checksums*, **Save**.
3. Press **Run now** on that job and watch the progress bar on the same page.
4. **Archive Explorer (Ctrl+4)** - confirm the files are listed at the destination.
5. **Restore (Ctrl+6)** - Load the job, select one file, choose a destination, **Restore
   selected**. This is the step that proves the copy is real.

Everything below is the same flow in more detail, page by page.

---

## 1. Dashboard (Ctrl+1)

The state of the whole installation at a glance: job counters, the last successful and failed
run, the next scheduled run, how many storage locations exist, and the most recent activity.

- **Re-check environment** re-probes Rclone and Restic, so after installing Rclone the warning
  disappears without restarting the application.
- The cards are read-only: this page tells you where to look, it does not change anything.

## 2. Backup Jobs (Ctrl+2)

The list of jobs, with their mode (archive or backup), enabled state, destination and last run.

- **New job...** opens the job editor (see below).
- **Edit...** changes every field of the selected job.
- **Delete** removes a job - it never touches files that were already archived or snapshotted.
- **Run now** starts the job immediately on a worker thread; the window stays responsive and the
  progress bar on this page follows the run.
- **Cancel** stops the running job cleanly: the file in flight is finished, nothing is left
  half-written.
- **History...** lists every run of the job with counters, duration and errors.

### The job editor

| Section | What to fill in |
|---|---|
| Basics | **Name**, and **Mode**: *Archive* (a copy that stays readable at the destination) or *Backup* (Restic snapshots: versioned and encrypted) |
| Source | One or more folders. Add as many as you need; each gets its own namespace at the destination |
| Destination | *Local folder* or *Storage provider*, plus an optional sub-path |
| Options | Exclusions (one glob per line), bandwidth limit, *Verify checksums*, *Enabled* |
| Schedule | Daily / weekly / monthly / on login / on startup, with the time; the next run is previewed live |
| Repository (backup only) | The Restic repository path, the retention policy (*keep last / daily / weekly / monthly / yearly*), *Check after backup*, **Set password** and **Initialise repository** |

Archive mode and backup mode are different things and are labelled as such: an archive is a
copy, a backup is history. If you want protection against a deletion or ransomware, use backup.

## 3. Storage (Ctrl+3)

Where jobs can send files.

- **Add local folder...** - a disk, a mount point, a network share that is already mounted.
- **Add Rclone remote...** - a cloud destination through Rclone. The remote name is what you gave
  `rclone config` (for example `icloud:`), plus an optional base path.
- **Test connection** proves the provider is reachable before a job needs it.
- **Remove** deletes the storage entry only; data at the provider is untouched.

If Rclone is missing, install it from Settings -> External tools first: Nesva never downloads
anything without showing you the steps.

## 4. Archive Explorer (Ctrl+4)

What a job actually put where: the destination listing joined with the manifest, so every row is
a real file at the destination with its hash. Useful as a sanity check after the first run, and
to find a file before restoring it.

## 5. Mail (Ctrl+5)

Microsoft 365 archival on one page. Connecting a mailbox is a browser sign-in, so it happens once
from a terminal (`nesva-cli mail connect`); after that everything is here.

- **Account** selector: the connected mailboxes.
- **Folder table**: per folder, how much mail Microsoft reports, how much is archived, how much is
  left, and a checkbox that includes or excludes the folder.
- **Estimate** counts the enabled folders and, once a sync has run, turns the measured speed into
  a duration.
- **Sync now** runs a real job: it creates the mail job on first use (asking for the archive
  directory), then shows the same progress, history and notifications as any other job.
- **Schedule job...** opens the normal job editor with the mail content preselected - daily or
  weekly archival is one dialog away.
- **Back up archive with Restic...** creates a *backup* job whose source is the mail archive
  directory, which gives the .eml files versioned, encrypted snapshots instead of only a plain
  copy.

## 6. Snapshots (Ctrl+6)

The Restic side of a backup job.

- **Refresh from repository** reads the snapshots that exist in the repository.
- **Browse files...** shows the files inside the selected snapshot without restoring it.
- **Restore snapshot...** restores a whole snapshot (see the Restore page for the conflict
  policy).
- **Retention preview** shows exactly which snapshots a policy would remove - and **Apply
  retention...** performs it. Nothing is removed before you have seen the list.
- **Check repository** verifies integrity; **Unlock** clears a stale lock left by an interrupted
  run.

## 7. Restore (Ctrl+7)

Restoring from an archive job or from a snapshot.

1. **Load** the job (or the snapshot).
2. Select the rows you want and pick a **restore destination folder** with **Browse...**.
3. Choose what happens to files that already exist: *Skip*, *Rename* or *Overwrite*. The default
   is skip; overwriting asks for confirmation.
4. **Restore selected** (or **Restore all**) and watch the progress.

Restored files are verified against the recorded hash, and a run that could not verify a file
says so instead of reporting success.

## 8. Activity (Ctrl+8)

Every job event and application event in one stream, with a text filter and sortable columns.
This is the first place to look when something went wrong; **Open log folder** takes you to the
JSON log if you need the detail.

## 9. Settings (Ctrl+9)

- **Appearance** - theme: follow system, light or dark.
- **External tools** - the status of Rclone and Restic, and **Install Rclone...**, which shows the
  download, the checksum check, the unpack command and the target path before you approve it
  (see [ADR 0014](../architecture/decisions/0014-external-tool-installation.md)).
- **Logging** - console verbosity.
- **Safety** - confirmation before destructive actions (keep it on).
- **Window behaviour** - minimise to tray, desktop notifications.
- **Background agent** - whether `nesva-agent` is running, **Refresh status**, **Stop agent**.
  The agent is what runs schedules when the window is closed.
- **Locations** - **Open data folder** and **Open log folder**.

---

## Where to look when something is wrong

| Symptom | Look at |
|---|---|
| A job did nothing | Jobs -> **History...**, then Activity for the same time range |
| "rclone not found" | Settings -> External tools; install it there |
| A scheduled run did not happen | Settings -> Background agent; the agent must be running |
| Restore refuses to overwrite | That is the default: pick *Rename* or *Overwrite* explicitly |
| Snapshots page is empty | **Refresh from repository**; check the repository path in the job |
| Anything unexplained | Activity page, then **Open log folder** |

For the same operations from a terminal, and for the archived-mail workflow, see the
[full tutorial](tutorial.md) and the [Microsoft 365 guide](microsoft-365.md).
