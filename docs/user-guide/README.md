# Nesva user guide

The user guide is completed in Phase 6 alongside the packaged installation. This file documents
what the current build actually does, so the documentation never runs ahead of the software.

## What you can do today (Phase 1)

1. Start the application: `nesva`.
2. Browse the eight pages from the sidebar (or press Ctrl+1 through Ctrl+8).
3. Read the Dashboard: job counters, last successful and failed runs, next scheduled run,
   storage providers, environment warnings (for example a missing Rclone installation) and the
   most recent activity.
4. Inspect the event stream on the Activity page and filter it by text.
5. Change the theme (system/light/dark), log verbosity and destructive-action confirmation on the
   Settings page. All settings are persisted.
6. Open the data or log folder directly from the Settings page.
7. Use `nesva-cli status` and `nesva-cli doctor` for diagnostics, including a JSON form.

Job creation, archive execution, restore and Restic backup arrive in Phase 2 and later. The
pages and commands for those phases state explicitly that the feature is not implemented yet -
nothing reports a false success.

## Where your data lives

- Linux: `$XDG_DATA_HOME/nesva` (default `~/.local/share/nesva`): `nesva.db` and `logs/`.
- Windows: `%LOCALAPPDATA%\Nesva`.
- Nothing is written next to the executable or into a system directory.

## Safety you can rely on today

- No file is ever deleted, moved or modified by this build.
- No network request is made.
- Logs contain no credentials: secrets are censored before they are written.
