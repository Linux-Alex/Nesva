# Building, testing and running Nesva

This guide covers a from-scratch build on Linux (primary development platform) and Windows
(primary target). All commands are copy-pasteable and were executed on the Linux machine
described below.

---

## 1. Prerequisites

### 1.1 Linux (Debian/Ubuntu)

Verified environment:

| Tool | Version used |
|---|---|
| Ubuntu | 26.04 LTS, x86_64 |
| GCC | 15.2.0 |
| CMake | 4.2.3 |
| Ninja | 1.13.2 |
| Qt | 6.10.2 (base, tools, SQL SQLite plugin) |
| SQLite | 3.46.1 (via Qt SQL plugin) |
| Rclone / Restic | not installed (optional for Phase 1) |

Install the dependencies:

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build \
                 qt6-base-dev qt6-base-dev-tools libqt6sql6-sqlite \
                 libgl1-mesa-dev git
```

Optional, for a Clang build and sanitizers:

```bash
sudo apt install clang lld
```

Verify:

```bash
gcc --version && cmake --version && ninja --version
qmake6 --version          # prints "Using Qt version 6.x.y"
pkg-config --modversion Qt6Widgets
```

### 1.2 Windows 10/11 x64

1. Install **Visual Studio 2022** with the "Desktop development with C++" workload (MSVC 19.3x).
2. Install **CMake** (3.25+) and **Ninja**.
3. Install **Qt 6.5+** for MSVC 2022 x64 - for example with the Qt online installer, selecting
   *MSVC 2022 64-bit* and the *Qt SQL* module.
4. Note the Qt install prefix (for example `C:/Qt/6.10.2/msvc2022_64`).

Windows builds must run from a **x64 Native Tools Command Prompt for VS 2022** so that MSVC and
Ninja are on `PATH`.

---

## 2. Configure, build, test

The presets are the supported interface. Do not hand-write long `cmake` invocations.

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug --output-on-failure
```

Release configuration:

```bash
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release --output-on-failure
```

Other presets:

| Preset | Purpose |
|---|---|
| `linux-debug` | GCC, Debug, warnings-as-errors, tests on |
| `linux-release` | GCC, Release (-O3), tests on |
| `linux-clang-debug` | Clang, Debug (requires clang on PATH) |
| `linux-asan` | Debug with AddressSanitizer + UndefinedBehaviorSanitizer |
| `windows-msvc-debug` | MSVC, Debug, Ninja (from a VS developer prompt) |
| `windows-msvc-release` | MSVC, Release, Ninja |

Configuration options (`-D` cache variables):

| Option | Default | Meaning |
|---|---|---|
| `NESVA_BUILD_TESTING` | `ON` | Build and register the test suite |
| `NESVA_WARNINGS_AS_ERRORS` | `OFF` (ON in presets) | Treat compiler warnings as errors |
| `NESVA_ENABLE_SANITIZERS` | `OFF` | ASan + UBSan (GCC/Clang only) |
| `NESVA_EMBED_GIT_COMMIT` | `OFF` | Embed the git commit hash in the version header |

On Windows, point CMake at Qt explicitly:

```bat
cmake --preset windows-msvc-debug -DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/msvc2022_64
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
```

### 2.1 What the build produces

```text
build/linux-debug/
├── nesva            # desktop application
├── nesva-cli        # automation and diagnostics
├── nesva-agent      # background scheduler and runner
├── libnesva_core.a  # core library
├── libnesva_ui.a    # widget library used by the app and the GUI tests
├── tests/tst_*      # test binaries
└── generated/nesva/version.hpp
```

---

## 3. Running the application

```bash
./build/linux-debug/nesva
```

Options:

| Option | Effect |
|---|---|
| `--data-dir <path>` | Use a specific data directory (useful for demos and tests) |
| `--log-level <level>` | Console log level: `trace`, `debug`, `info`, `warn`, `error` |
| `--no-console-log` | Write logs to the log file only |
| `--gui-selftest[=<dir>]` | Show the window, capture every page as PNG, then exit |
| `--version`, `--help` | Version information / usage |

The database, logs and cache are created in the per-user data directory on first start:

- Linux: `$XDG_DATA_HOME/nesva` (default `~/.local/share/nesva`)
- Windows: `%LOCALAPPDATA%\Nesva`

Override order: `--data-dir`, then `NESVA_DATA_DIR`, then the platform default. Nothing is
written next to the executable.

### 3.1 Inspecting state

```bash
./build/linux-debug/nesva-cli status
./build/linux-debug/nesva-cli doctor
./build/linux-debug/nesva-cli --json jobs list
./build/linux-debug/nesva-cli history list --limit 20
tail -n 20 "${XDG_DATA_HOME:-$HOME/.local/share}/nesva/logs/nesva.log"
```

Log lines are newline-delimited JSON:

```json
{"category":"cli","fields":{"command":"status","subcommand":""},"level":"info","message":"command invoked","timestamp":"2026-11-02T19:27:48.331Z"}
```

---

## 4. Testing

```bash
ctest --preset linux-debug --output-on-failure      # everything
ctest --preset linux-debug -R tst_database -V       # one suite, verbose output
./build/linux-debug/tests/tst_redactor -functions   # list test functions
```

Current suites: `tst_result`, `tst_redactor`, `tst_logging`, `tst_paths`,
`tst_database`, `tst_settings_store`, `tst_external_tool`, `tst_gui_smoke`.

Tests always run against temporary directories; they never touch a real user data directory and
never modify user files. GUI tests run with `QT_QPA_PLATFORM=offscreen` (set by CTest).

### 4.1 GUI verification

Two complementary mechanisms:

1. **Automated** - `tst_gui_smoke` builds the real window and reads seeded rows headlessly.
2. **Visual artifacts** - the application's own self-test mode:

```bash
# Headless
QT_QPA_PLATFORM=offscreen ./build/linux-debug/nesva \
    --data-dir=/tmp/nesva-demo --gui-selftest=/tmp/nesva-shots --no-console-log

# Real session (X11 example)
DISPLAY=:0 ./build/linux-debug/nesva \
    --data-dir=/tmp/nesva-demo --gui-selftest=/tmp/nesva-shots --no-console-log
```

Output:

```text
page 1/8: Dashboard          gui-selftest-1-dashboard.png
...
GUI self-test: 8 of 8 pages captured into /tmp/nesva-shots
```

A non-zero exit code means a page failed to render or could not be saved.

---

## 5. Static analysis and formatting

- `compile_commands.json` is exported to every build directory (`CMAKE_EXPORT_COMPILE_COMMANDS`),
  so `clang-tidy` and `clangd` work without extra configuration:

  ```bash
  clang-tidy -p build/linux-debug src/core/logging.cpp
  ```

- Formatting configuration lives in `.clang-format` (LLVM base, 4-space indent, 100 columns),
  and editor defaults in `.editorconfig`:

  ```bash
  clang-format --dry-run --Werror src/**/*.cpp include/nesva/**/*.hpp
  ```

- Sanitizer preset (`linux-asan`) builds and runs the same tests with ASan + UBSan.

---

## 6. Installing and packaging

Install into a staging prefix:

```bash
cmake --install build/linux-release --prefix /tmp/nesva-stage
```

This installs `nesva`, `nesva-cli`, the desktop entry
(`share/applications/nesva.desktop`) and the scalable icon.
mutable application data is never installed there.

CPack is configured (`CPACK_GENERATOR=DEB` on Linux, `WIX` on Windows) for the distribution
phase; full installers, AppImage/RPM artifacts and Windows packaging are Phase 6 deliverables.

---

## 7. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `Could not find a package configuration file provided by "Qt6"` | Qt is not installed or not on `CMAKE_PREFIX_PATH`. Set `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.x.y/<toolchain>` |
| `Qt SQLite driver (QSQLITE) is not available` | Install `libqt6sql6-sqlite` (Linux) or the *Qt SQL* module (Windows) |
| `qt.qpa.plugin: could not load the Qt platform plugin "xcb"` | Missing X11 libs on a headless host - use `QT_QPA_PLATFORM=offscreen` |
| `external tool not found (rclone)` in `doctor` | Rclone/Restic are not installed; only cloud/backup features are affected |
| Schema error "database is newer than this application build" | An older binary is pointed at a newer database. Use a build from the same commit tree |
| Build fails after a compiler upgrade with warnings-as-errors | Clean the build directory, or configure with `-DNESVA_WARNINGS_AS_ERRORS=OFF` to see all warnings |

Log files and `nesva-cli doctor` output are the fastest way to diagnose an environment problem.
Never paste secrets into an issue; the redactor protects logs, not your terminal.

---

## 8. Continuous integration

GitHub Actions runs (see `.github/workflows`): Linux debug build + tests + GUI self-test, Linux
release build + tests, Windows x64 build + tests, static analysis, dependency review, and
release packaging with checksums. The workflows are the executable version of this guide.

### Cutting a release

`.github/workflows/release.yml` runs on a `v*` tag (and on demand without publishing):

| Platform | Artifacts |
| --- | --- |
| Linux x86_64 | `nesva_<version>_amd64.deb`, `nesva-<version>-linux-x86_64.tar.gz` (portable prefix tree), `SHA256SUMS-linux.txt` |
| Windows x86_64 | `nesva-<version>-windows-x86_64-setup.exe` (per-user NSIS installer, `packaging/windows/nesva.nsi`), `nesva-<version>-windows-x86_64.zip` (three executables, Qt runtime and plugins, licence, notes), `SHA256SUMS-windows.txt` |

The Linux packages **bundle the Qt runtime**: the release is built against a Qt newer
than any distribution ships, so a package depending on the system Qt would install and then fail to
start. `cmake/DeployQtRuntime.cmake.in` copies the Qt libraries the binaries need (walked through
`DT_NEEDED` along `libQt6*` edges only) plus the platform, SQLite and TLS plugins into
`lib/nesva`, writes `bin/qt.conf`, and relies on the link-time rpath
(`CMAKE_INSTALL_RPATH`) to point at it. Everything Qt itself needs from the system - X11,
fontconfig, xkbcommon - stays a package dependency that `dpkg-shlibdeps` derives from the files.

Both jobs run the whole test suite first and smoke-test the assembled package: the Linux tarball
is unpacked and the GUI self-test runs from it, the Windows zip is expanded and checked for
`Qt6Core.dll` before `nesva.exe` self-tests from the extracted folder, and the Windows installer is
installed unattended into a temporary directory, exercised, and uninstalled again. The `publish`
job then creates the GitHub release and uploads the assets with the checksums.

```bash
# 1. bump the version in CMakeLists.txt and move the Unreleased notes in CHANGELOG.md
# 2. commit, then tag and push
git tag -a v0.6.4 -m "Nesva 0.6.4"
git push origin v0.6.4
```

Re-running the workflow for an existing tag uploads the new assets over the old ones
(`gh release upload --clobber`). Nothing is code-signed yet, so Windows SmartScreen warns on the
first launch of the zip.
