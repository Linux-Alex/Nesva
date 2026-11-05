# Linux packaging

Phase 1 installs the application with plain CMake (see `docs/developer-guide/building.md`):

```bash
cmake --preset linux-release
cmake --build --preset linux-release
cmake --install build/linux-release --prefix /tmp/nesva-stage
```

That installs everything Nesva owns into its own directory and leaves symlinks behind:

```text
<prefix>/lib/nesva/bin/nesva          the real executables
<prefix>/lib/nesva/bin/nesva-cli
<prefix>/lib/nesva/bin/nesva-agent
<prefix>/lib/nesva/bin/qt.conf        tells Qt where Nesva's plugins are
<prefix>/lib/nesva/lib/libQt6*.so.6   the bundled Qt runtime
<prefix>/lib/nesva/plugins/           the plugins Nesva loads
<prefix>/bin/nesva -> ../lib/nesva/bin/nesva
<prefix>/share/applications/nesva.desktop
<prefix>/share/icons/hicolor/scalable/apps/nesva.svg
```

**Nothing may be installed into a shared system directory**, and in particular no `qt.conf` may
sit in `/usr/bin`: Qt resolves `qt.conf` relative to the executable, so a file there applies to
*every* Qt program started from that directory. Nesva 0.6.4 shipped exactly that and broke KDE
Spectacle, which then looked for its plugins in `/usr/lib/nesva/plugins`.

The executables carry `DT_RPATH = $ORIGIN/../lib`, so the bundled runtime is found through the
binary itself - never through `/etc/ld.so.conf.d`, `QT_PLUGIN_PATH`, `QML_IMPORT_PATH` or
`LD_LIBRARY_PATH`, which would change behaviour for other applications.

`scripts/check-linux-package.sh <package.deb>` enforces all of the above on a built package, and
`scripts/test-deb-upgrade.sh <old.deb> <new.deb> [image]` verifies an install, an upgrade and a
removal inside a container.

CPack is configured with the `DEB` generator (`CPACK_DEBIAN_FILE_NAME=DEB-DEFAULT`,
`CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON`), so

```bash
cd build/linux-release && cpack
```

produces a `.deb` from the install rules.

## Phase 6 scope

- `.deb` with dependencies on `libqt6widgets6`, `libqt6sql6-sqlite` and `libqt6network6`;
- AppImage built from the staging prefix with the Qt runtime bundled;
- `.rpm` generated on a platform with rpmbuild available (present on this development machine);
- optional **user-level** systemd unit for `nesva-agent` (`systemctl --user` - never a system-wide
  root service, because cloud and keyring credentials are per user);
- documentation of the XDG paths actually used: `~/.local/share/nesva` for the database and
  logs, `~/.cache/nesva` for bounded temporary files.

Mutable data is never installed into the package prefix: the package ships only immutable files.

The generated .deb derives its Qt library dependencies with dpkg-shlibdeps. The Qt SQLite driver
is loaded at runtime through the Qt plugin mechanism and is therefore not detected
automatically: Phase 6 packaging adds libqt6sql6-sqlite to the dependency list explicitly
(together with rclone/restic as recommendations).
