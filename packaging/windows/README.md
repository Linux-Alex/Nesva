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
autostart**, next to **Start agent** (start it now without waiting for the next logon) and
**Stop agent**.

The agent is a GUI-subsystem binary, so logging in does not open a console window in your
session. Started from a terminal it attaches to that console, so `nesva-agent --help` and
`nesva-agent --verbose` still print there. Everything else goes to the log file.

* No elevation is required and none is requested.
* No PowerShell, no WScript, no scheduled task, so script-blocking group policies do not apply.
* The value points at the absolute path of `nesva-agent.exe`, so an update that moves the
  executable has to run `agent install` again.
* Removing Nesva does not delete the Run entry; run `nesva-cli agent uninstall` first.

## Requirements

* Rclone and Restic are not bundled. Install them from **Settings -> External tools -> Install
  Rclone... / Install Restic...**: it
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

Each release publishes `nesva-<version>-windows-x86_64.zip`: `nesva.exe`, `nesva-cli.exe`,
`nesva-agent.exe`, the Qt runtime and plugins deployed with `windeployqt`, the licence and these
notes. Unpack it anywhere and run `nesva.exe` - no installer, no elevation, no registry writes
apart from the optional autostart value above. `SHA256SUMS-windows.txt` sits next to the zip in the
release.

The binaries are **unsigned**: SmartScreen warns on first launch and cannot be silenced without a
code-signing certificate. A CPack NSIS target exists in the top-level CMakeLists, but it is not
built or tested here, so the zip is the supported artefact.

Build it yourself with the `windows-msvc-release` preset, or run the `Release` workflow
(`workflow_dispatch` produces the artifacts without publishing them).
