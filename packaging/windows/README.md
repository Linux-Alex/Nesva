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

## Icon and version metadata

`assets/icons/nesva.ico` is the Windows icon (16-256 px) drawn from the same design as
`assets/icons/nesva.svg`. `packaging/windows/nesva.rc.in` embeds it in `nesva.exe` together with
a `VERSIONINFO` block, so Explorer, shortcuts and the taskbar show the Nesva icon and File
Properties shows the real version; `QApplication::setWindowIcon()` alone only affects the
windows at runtime. CMake configures the template into the build tree so the icon path is
absolute.

## Packaging

Each release publishes two artefacts, both built from the same staged directory (the three
executables, the Qt runtime and plugins deployed with `windeployqt`, the CRT DLLs, the licence and
these notes):

| File | What it is |
| --- | --- |
| `nesva-<version>-windows-x86_64-setup.exe` | Installer (`packaging/windows/nesva.nsi`): installs into `%LOCALAPPDATA%\Programs\Nesva`, Start-menu shortcut, optional desktop shortcut, entry in *Apps & features*, uninstaller |
| `nesva-<version>-windows-x86_64.zip` | Portable: unpack anywhere and run `nesva.exe` |

Both are **per-user**: no administrator rights, no UAC prompt, no machine-wide state.

The uninstaller removes the binaries, the bundled Qt runtime, the shortcuts and the logon autostart
value, and leaves your data alone on purpose - the profile database stays in
`%LOCALAPPDATA%\nesva` and the archives stay in whatever destinations the jobs were given.

`SHA256SUMS-windows.txt` sits next to both files in the release. The binaries are **unsigned**:
SmartScreen warns on first launch and cannot be silenced without a code-signing certificate.

The Release workflow builds both and smoke-tests the installer before publishing: unattended
install into a temporary directory, run the installed `nesva-cli` and the GUI self-test from
there, then unattended uninstall and a check that nothing was left behind. Run it by hand with
`workflow_dispatch` to get the artifacts without publishing them.

Rebuild the installer locally:

```powershell
choco install nsis -y
makensis /DVERSION=0.6.1 /DSTAGE=C:\stage /DOUTFILE=C:\out\nesva-setup.exe packaging\windows\nesva.nsi
```
