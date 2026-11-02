# Windows packaging

Target platform: Windows 10/11 x64 (primary Nesva target).

## Phase 1 status

The build itself is ready (`windows-msvc-release` preset, `WIN32_EXECUTABLE` application, CPack
with the `WIX` generator configured). No installer is produced yet, and **no Windows build has
been executed or verified** - the development machine is Linux. Windows CI
(`.github/workflows/ci.yml`) is the first place a real MSVC build will run.

## Phase 6 scope

- WiX-based MSI (or an EXE wrapper) produced by CPack from a clean Release build;
- Start menu entry, optional desktop shortcut, uninstaller with per-user data preserved;
- Qt runtime deployment through `windeployqt` (Core, Gui, Widgets, Sql, Network, the `qsqlite`
  SQL driver plugin and the `platforms` plugins);
- application data in `%LOCALAPPDATA%\Nesva` for mutable state; **never** in `Program Files`;
- optional registration of `nesva-agent` as a per-user scheduled task
  (`schtasks /Create /SC ONLOGON /RL LIMITED /TN NesvaAgent ...`). Windows SYSTEM does not have
  access to an interactive user's cloud credentials or keyring, so the agent must run in the
  user's own context;
- code signing documentation and checksums for every published artifact.

## Cross-compilation note

Cross-building Qt 6 Widgets applications from Linux to Windows is not supported by this project.
Windows artifacts are produced on Windows (CI runner or a developer machine).
