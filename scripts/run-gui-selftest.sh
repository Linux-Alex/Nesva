#!/usr/bin/env bash
# Runs the Nesva GUI self-test and reports where the page screenshots were written.
#
# Usage: scripts/run-gui-selftest.sh [build-directory] [output-directory]
#
# Defaults: build/linux-debug and a temporary directory under /tmp.
# Uses the current graphical session when available, otherwise the offscreen
# platform plugin (so it also works in CI and over SSH).

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

build_dir="${1:-${repo_root}/build/linux-debug}"
output_dir="${2:-$(mktemp -d /tmp/nesva-gui-selftest.XXXXXX)}"

app="${build_dir}/nesva"
if [ ! -x "$app" ]; then
    echo "error: $app not found - build first: cmake --build --preset linux-debug" >&2
    exit 1
fi

if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    platform_args=()
else
    export QT_QPA_PLATFORM=offscreen
    platform_args=()
fi

mkdir -p "$output_dir"
"$app" --data-dir="$output_dir/data" --gui-selftest="$output_dir" --no-console-log "${platform_args[@]}"

echo
echo "Screenshots: $output_dir"
ls -1 "$output_dir"/*.png
