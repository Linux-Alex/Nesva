#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2025 The Nesva Authors
#
# Verifies a built Linux .deb before it is allowed into a release.
#
# The rules enforced here come from a real incident: Nesva 0.6.4 shipped a qt.conf
# in /usr/bin, and because Qt resolves qt.conf relative to the executable, every Qt
# program started from /usr/bin inherited the plugin and library paths of Nesva.
# KDE Spectacle then looked for its Wayland and QtQuick plugins in
# /usr/lib/nesva/plugins and crashed. A package must never reach that state again,
# so these checks fail the build instead of a user desktop.
#
# Usage: scripts/check-linux-package.sh <package.deb>
set -euo pipefail

deb="${1:-}"
if [ -z "$deb" ] || [ ! -f "$deb" ]; then
    echo "usage: $0 <package.deb>" >&2
    exit 2
fi

failures=0
pass() { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; failures=$((failures + 1)); }

names="$(dpkg-deb --fsys-tarfile "$deb" | tar -t)"
echo "Checking $deb"

# 1. Nothing may write Qt configuration into a shared executables directory.
if echo "$names" | grep -qx "./usr/bin/qt.conf"; then
    fail "/usr/bin/qt.conf is back: it breaks every other Qt application"
else
    pass "/usr/bin/qt.conf is not installed"
fi
stray_qt_conf="$(echo "$names" | grep -F "qt.conf" | grep -vF "./usr/lib/nesva/bin/qt.conf" || true)"
if [ -z "$stray_qt_conf" ]; then
    pass "no qt.conf outside the private bin directory"
else
    fail "qt.conf installed outside the private directory:"
    echo "$stray_qt_conf" | sed "s/^/          /" >&2
fi

# 2. No system library or plugin directory is written to.
stray_system="$(echo "$names" | grep -F "./usr/lib/" | grep -vF "./usr/lib/nesva/" | grep -vx "./usr/lib/" || true)"
if [ -z "$stray_system" ]; then
    pass "no files in system library or Qt plugin directories"
else
    fail "the package writes into a system library directory:"
    echo "$stray_system" | sed "s/^/          /" >&2
fi
stray_qt_data="$(echo "$names" | grep -F "./usr/share/qt" || true)"
if [ -z "$stray_qt_data" ]; then
    pass "no files in the system Qt data directories"
else
    fail "the package writes into the system Qt data directories:"
    echo "$stray_qt_data" | sed "s/^/          /" >&2
fi

# 3. No loader configuration for other applications.
ld_conf="$(echo "$names" | grep -F "./etc/ld.so.conf.d/" || true)"
if [ -z "$ld_conf" ]; then
    pass "no /etc/ld.so.conf.d entries"
else
    fail "the package installs a system-wide loader path:"
    echo "$ld_conf" | sed "s/^/          /" >&2
fi

workdir="$(mktemp -d)"
trap "rm -rf $workdir" EXIT
dpkg-deb -e "$deb" "$workdir/control"
dpkg-deb -x "$deb" "$workdir/root"

for hook in postinst preinst prerm postrm; do
    [ -f "$workdir/control/$hook" ] || continue
    if grep -qE "QT_PLUGIN_PATH|QML_IMPORT_PATH|QT_QPA_PLATFORM_PLUGIN_PATH|ld.so.conf" "$workdir/control/$hook"; then
        fail "$hook sets a system-wide Qt path"
    else
        pass "$hook sets no system-wide Qt path"
    fi
done

# 4. The launcher and the bundled runtime must be where the desktop entry expects.
launcher="$workdir/root/usr/bin/nesva"
if [ -L "$launcher" ]; then
    target="$(readlink "$launcher")"
    if [ "$target" = "../lib/nesva/bin/nesva" ]; then
        pass "/usr/bin/nesva is a symlink into the private directory"
    else
        fail "/usr/bin/nesva points at $target, expected ../lib/nesva/bin/nesva"
    fi
elif [ -f "$launcher" ]; then
    fail "/usr/bin/nesva is a real binary; a shared executables directory may not carry Nesva qt.conf"
else
    fail "/usr/bin/nesva is missing"
fi

private_bin="$workdir/root/usr/lib/nesva/bin"
if [ -f "$private_bin/qt.conf" ]; then
    if grep -qxF "[Paths]" "$private_bin/qt.conf" &&
       grep -qxF "Plugins = plugins" "$private_bin/qt.conf" &&
       grep -qxF "Libraries = lib" "$private_bin/qt.conf" &&
       grep -qxF "Prefix = .." "$private_bin/qt.conf"; then
        pass "the private qt.conf points at the bundled plugins and libraries"
    else
        fail "the private qt.conf has unexpected contents:"
        sed "s/^/          /" "$private_bin/qt.conf" >&2
    fi
else
    fail "the private qt.conf is missing: Qt would fall back to the system plugins"
fi

desktop="$workdir/root/usr/share/applications/nesva.desktop"
if [ -f "$desktop" ]; then
    exec_program="$(grep -m1 -F "Exec=" "$desktop" | sed "s/^Exec=//" | cut -d" " -f1)"
    if [ -e "$workdir/root/usr/bin/$exec_program" ]; then
        pass "the desktop entry starts a program the package provides ($exec_program)"
    else
        fail "the desktop entry starts $exec_program, which the package does not provide"
    fi
else
    fail "the desktop entry is missing"
fi

# 5. Every installed binary must resolve its libraries, Qt included, from the
#    package itself. LD_LIBRARY_PATH is cleared so a Qt on the build machine cannot
#    make a broken package look healthy.
for tool in nesva nesva-cli nesva-agent; do
    binary="$workdir/root/usr/lib/nesva/bin/$tool"
    if [ ! -x "$binary" ]; then
        fail "$tool is missing from the private bin directory"
        continue
    fi
    if env -u LD_LIBRARY_PATH ldd "$binary" 2>/dev/null | grep -q "not found"; then
        fail "$tool has unresolved shared libraries:"
        env -u LD_LIBRARY_PATH ldd "$binary" | grep "not found" | sed "s/^/          /" >&2
        continue
    fi
    # ldd prints the path it used, which still contains "bin/../lib"; resolve the
    # binary first so the comparison sees the real directory.
    resolved="$(cd "$(dirname "$binary")" && env -u LD_LIBRARY_PATH ldd "$(basename "$binary")" | grep "Qt6Core" || true)"
    if echo "$resolved" | grep -qF "libQt6Core.so.6" &&
       ! echo "$resolved" | grep -qF "linux-gnu"; then
        pass "$tool resolves Qt from the bundled runtime"
    else
        fail "$tool resolves Qt from outside the package: $resolved"
    fi
done

# 6. The executable must find the runtime through its own rpath, not through the
#    loader cache: DT_RPATH is searched before LD_LIBRARY_PATH.
dynamic="$(readelf -d "$workdir/root/usr/lib/nesva/bin/nesva")"
if echo "$dynamic" | grep -q "(RPATH)" && echo "$dynamic" | grep -F "/../lib"; then
    pass "the executable carries an ORIGIN rpath into the bundled runtime"
else
    fail "missing or unexpected RPATH in the installed executable"
fi
if echo "$dynamic" | grep -q "(RUNPATH)"; then
    fail "the executable uses DT_RUNPATH, which LD_LIBRARY_PATH can override"
else
    pass "the executable uses DT_RPATH, not DT_RUNPATH"
fi

# 7. Bundled Qt means no Qt package dependency. A build against the distribution Qt
#    produces no bundle at all, and dpkg-shlibdeps then legitimately names the Qt
#    packages; that local case is what NESVA_ALLOW_SYSTEM_QT_DEPS is for. Releases
#    are built against the official Qt and must never need it.
if ! dpkg-deb -f "$deb" Depends | grep -qE "(^|, )libqt6"; then
    pass "no Qt package dependency (the runtime is bundled)"
elif [ -n "${NESVA_ALLOW_SYSTEM_QT_DEPS:-}" ]; then
    echo "  note  the package depends on the distribution Qt (allowed for local builds)"
else
    fail "the package depends on the distribution Qt while bundling its own"
fi

# 8. The obsolete global file must be handled by a maintainer script, for the
#    upgrades dpkg does not fully clean up.
if [ -f "$workdir/control/postinst" ] && grep -q "/usr/bin/qt.conf" "$workdir/control/postinst"; then
    pass "postinst carries the migration for the leftover global qt.conf"
else
    fail "postinst does not handle the leftover global qt.conf"
fi

if [ "$failures" -ne 0 ]; then
    echo
    echo "$failures packaging check(s) failed" >&2
    exit 1
fi

echo
echo "All Linux packaging checks passed."
