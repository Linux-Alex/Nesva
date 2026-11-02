#!/usr/bin/env bash
# Reports whether the local machine can build and test Nesva.
#
# Usage: scripts/dev-env-check.sh
# Exit code: 0 when every required dependency is present, 1 otherwise.

set -u

required_missing=0

report() {
    local name="$1" status="$2" detail="$3"
    printf '%-22s %-6s %s\n' "$name" "$status" "$detail"
}

have() { command -v "$1" >/dev/null 2>&1; }

echo "Nesva development environment check"
echo "------------------------------------------------------------------"

if have g++ || have clang++; then
    cxx=$(have g++ && echo g++ || echo clang++)
    report "c++ compiler" "ok" "$($cxx --version | head -n 1)"
else
    report "c++ compiler" "FAIL" "install build-essential or clang"
    required_missing=1
fi

if have cmake; then
    version=$(cmake --version | head -n 1 | awk '{print $3}')
    if [ "$(printf '%s\n' "$version" | cut -d. -f1)" -ge 3 ] 2>/dev/null; then
        report "cmake" "ok" "$version"
    else
        report "cmake" "FAIL" "$version (requires 3.25+ or a config preset capable CMake)"
        required_missing=1
    fi
else
    report "cmake" "FAIL" "install cmake"
    required_missing=1
fi

if have ninja; then
    report "ninja" "ok" "$(ninja --version)"
else
    report "ninja" "FAIL" "install ninja-build"
    required_missing=1
fi

qt_version=$(pkg-config --modversion Qt6Core 2>/dev/null || true)
if [ -n "$qt_version" ]; then
    report "qt6" "ok" "$qt_version"
    for module in Qt6Widgets Qt6Sql Qt6Test; do
        if pkg-config --exists "$module"; then
            report "  $module" "ok" "found"
        else
            report "  $module" "FAIL" "install the matching Qt module"
            required_missing=1
        fi
    done
else
    report "qt6" "FAIL" "install qt6-base-dev and qt6-base-dev-tools"
    required_missing=1
fi

driver_path=$(find /usr/lib/*/qt6/plugins/sqldrivers -name 'libqsqlite.so' 2>/dev/null | head -n 1)
if [ -n "$driver_path" ]; then
    report "qt sqlite driver" "ok" "$driver_path"
else
    report "qt sqlite driver" "WARN" "install libqt6sql6-sqlite"
fi

for tool in rclone restic; do
    if have "$tool"; then
        report "$tool" "ok" "$($tool version 2>/dev/null | head -n 1)"
    else
        report "$tool" "WARN" "not installed - cloud/backup features stay unavailable"
    fi
done

if have clang-tidy; then
    report "clang-tidy" "ok" "$(clang-tidy --version | sed -n 2p)"
else
    report "clang-tidy" "WARN" "not installed - static analysis runs in CI only"
fi

if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    report "graphical session" "ok" "DISPLAY=${DISPLAY:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-}"
else
    report "graphical session" "WARN" "no display detected - use QT_QPA_PLATFORM=offscreen"
fi

echo "------------------------------------------------------------------"
if [ "$required_missing" -eq 0 ]; then
    echo "Result: ready to build. Next: cmake --preset linux-debug && cmake --build --preset linux-debug"
    exit 0
fi
echo "Result: missing required dependencies."
exit 1
