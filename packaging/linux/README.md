# Linux packaging

Phase 1 installs the application with plain CMake (see `docs/developer-guide/building.md`):

```bash
cmake --preset linux-release
cmake --build --preset linux-release
cmake --install build/linux-release --prefix /tmp/nesva-stage
```

That installs:

```text
<prefix>/bin/nesva
<prefix>/bin/nesva-cli
<prefix>/share/applications/nesva.desktop
<prefix>/share/icons/hicolor/scalable/apps/nesva.svg
```

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
