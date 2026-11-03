# Windows: background agent and packaging

Nesva runs scheduled jobs through `nesva-agent`, a per-user background process. It must run as the
user whose data is backed up - never as SYSTEM: SYSTEM has its own profile and cannot see the
interactive user's keyring.

## Autostart (no PowerShell, no scheduled task)

Group policies frequently block PowerShell scripts and `schtasks`, so Nesva uses neither. The agent
is registered as a **per-user Run entry**, which the Windows shell executes at logon in the
interactive session, with the user's own token and without elevation:

```
nesva-cli agent install      # writes HKCU\Software\Microsoft\Windows\CurrentVersion\Run
nesva-cli agent uninstall     # removes it again
nesva-cli agent status        # is it running?
```

The same is available in the desktop application: **Settings -> Background agent -> Install
autostart**.

* No elevation is required and none is requested.
* No PowerShell, no WScript, no scheduled task, so script-blocking group policies do not apply.
* The value points at the absolute path of `nesva-agent.exe`, so an update that moves the
  executable has to run `agent install` again.
* Removing Nesva does not delete the Run entry; run `nesva-cli agent uninstall` first.

## Requirements

* Rclone is not bundled. Install it from **Settings -> External tools -> Install Rclone...**: it
  shows every step, verifies the published SHA-256 and needs no admin rights.
* Restic is not bundled either; download the official binary, verify its checksum and put it on
  `PATH` (or point to it in Settings).
* Rclone needs the remote to exist: **Storage -> Configure Rclone...** opens `rclone config` in a
  terminal.

## Verifying an installation

```
nesva-cli doctor
nesva-cli agent install
nesva-cli agent status
nesva.exe --gui-selftest=%TEMP%\nesva-shots
```

`doctor` reports the user context (and warns when the process runs as a service account), the data
directory, free space and both external tools.

## Packaging

There is no signed installer yet. Build from source with the `windows-msvc-release` preset, or use
the artefacts of the CI job `windows-msvc`. A CPack NSIS target exists in the top-level CMakeLists,
but it has not been built or tested on Windows in this repository, and the binaries are unsigned.
