#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2025 The Nesva Authors
#
# End-to-end install / upgrade / removal test for the Linux .deb, in a container.
#
# The point is the migration: Nesva 0.6.4 installed a qt.conf into /usr/bin, which
# made every Qt program started from that directory look for its plugins in the
# Nesva private directory. This installs the old package, upgrades to the new one,
# then checks that the global file is gone, that Nesva still runs, and that a system
# Qt tool reports the system plugin directory again.
#
# Usage: scripts/test-deb-upgrade.sh <old.deb> <new.deb> [image]
set -euo pipefail

old_deb="$(realpath "${1:?usage: check old.deb}")"
new_deb="$(realpath "${2:?usage: check new.deb}")"
image="${3:-ubuntu:24.04}"
inner_dir="$(mktemp -d)"
trap "rm -rf $inner_dir" EXIT

echo "Old package: $old_deb"
echo "New package: $new_deb"
echo "Image:       $image"

cat > "$inner_dir/inside.sh" <<"INSIDE"
#!/usr/bin/env bash
set -euo pipefail
old_deb="$1"
new_deb="$2"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates >/dev/null
# qtpaths6 belongs to the system Qt and is what a distribution Qt application uses
# to locate its plugins; qt6-qpa-plugins provides the platform plugins whose loss
# was the visible symptom (Spectacle could not open a Wayland surface).
apt-get install -y -qq --no-install-recommends qt6-base-dev-tools >/dev/null ||     apt-get install -y -qq --no-install-recommends qt6-base-dev-tools qt6-base-private-dev >/dev/null
apt-get install -y -qq --no-install-recommends qt6-qpa-plugins >/dev/null || true

system_plugins_before="$(qtpaths6 --query QT_INSTALL_PLUGINS)"
echo "system plugin dir before: $system_plugins_before"
if [ -d "$system_plugins_before/platforms" ]; then
    echo "system platform plugins present before the installation"
else
    echo "note: this image ships no system Qt platform plugins; that assertion is skipped"
fi

echo "--- installing the old package ---"
apt-get install -y -qq "$old_deb" >/dev/null
if [ ! -f /usr/bin/qt.conf ]; then
    echo "FAIL: the old package did not install /usr/bin/qt.conf, so this test proves nothing"
    exit 1
fi
echo "old nesva version: $(dpkg-query -W nesva | cut -f2)"
echo "global qt.conf owner: $(dpkg -S /usr/bin/qt.conf)"

echo "--- upgrading ---"
apt-get install -y -qq "$new_deb" >/dev/null
echo "new nesva version: $(dpkg-query -W nesva | cut -f2)"

failures=0
check() { if eval "$2"; then echo "  ok    $1"; else echo "  FAIL  $1"; failures=$((failures + 1)); fi; }

check "the global /usr/bin/qt.conf is gone" "[ ! -e /usr/bin/qt.conf ]"
check "no qt.conf left in the PATH directories" "[ ! -e /usr/local/bin/qt.conf ]"
check "/usr/bin/nesva is a symlink into the private tree" "[ -L /usr/bin/nesva ]"
check "the private tree holds the binary" "[ -x /usr/lib/nesva/bin/nesva ]"
check "the private qt.conf exists" "[ -f /usr/lib/nesva/bin/qt.conf ]"
check "the desktop entry is installed" "[ -f /usr/share/applications/nesva.desktop ]"

export QT_QPA_PLATFORM=offscreen
check "nesva-cli runs" "nesva-cli --version >/dev/null"
check "the GUI starts and draws its pages" "nesva --data-dir /tmp/nesva-data --gui-selftest /tmp/nesva-shots --no-console-log >/dev/null 2>&1"
check "the GUI self-test wrote screenshots" "ls /tmp/nesva-shots/ | grep -q png"

system_plugins_after="$(qtpaths6 --query QT_INSTALL_PLUGINS)"
echo "system plugin dir after:  $system_plugins_after"
check "a system Qt tool still reports the system plugin directory" "[ "$system_plugins_after" = "$system_plugins_before" ]"
# The directory existing is what matters; which plugin subdirectories a minimal image
# ships depends on the packages installed there.
check "the system plugin directory still exists" "[ -d "$system_plugins_after" ]"
check "the system platform plugins are still there" "[ ! -d "$system_plugins_before/platforms" ] || [ -d "$system_plugins_after/platforms" ]"

echo "--- removing ---"
apt-get remove -y -qq nesva >/dev/null
check "removal leaves no binary tree" "[ ! -e /usr/lib/nesva ]"
check "removal leaves no launcher" "[ ! -e /usr/bin/nesva ]"
check "removal leaves no global qt.conf" "[ ! -e /usr/bin/qt.conf ]"
check "the system plugin directory survives removal" "[ -d "$system_plugins_after" ]"

if [ "$failures" -ne 0 ]; then
    echo "$failures upgrade check(s) failed"
    exit 1
fi
echo "All upgrade checks passed."
INSIDE

chmod +x "$inner_dir/inside.sh"
docker run --rm -v "$(dirname "$old_deb"):/old:ro" -v "$(dirname "$new_deb"):/new:ro" \
    -v "$inner_dir:/inside:ro" "$image" \
    bash /inside/inside.sh "/old/$(basename "$old_deb")" "/new/$(basename "$new_deb")"
