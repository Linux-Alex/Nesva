#!/usr/bin/env bash
# Installs the Nesva background agent as a systemd *user* service.
#
# Usage: scripts/install-agent-service.sh [path-to-nesva-agent]
#
# The service is installed for the current user only: scheduled jobs need the
# user's cloud credentials and keyring, which a system-wide service cannot see.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

agent_binary="${1:-${repo_root}/build/linux-debug/nesva-agent}"
if [ ! -x "${agent_binary}" ]; then
    echo "error: ${agent_binary} not found - build first:" >&2
    echo "       cmake --preset linux-debug && cmake --build --preset linux-debug" >&2
    exit 1
fi

install_dir="${HOME}/.local/bin"
unit_dir="${HOME}/.config/systemd/user"
mkdir -p "${install_dir}" "${unit_dir}"

install -m 0755 "${agent_binary}" "${install_dir}/nesva-agent"
install -m 0644 "${repo_root}/packaging/linux/nesva-agent.service" "${unit_dir}/nesva-agent.service"

systemctl --user daemon-reload
systemctl --user enable --now nesva-agent.service

echo "Installed: ${install_dir}/nesva-agent"
echo "Unit:      ${unit_dir}/nesva-agent.service"
echo
systemctl --user status nesva-agent.service --no-pager || true
echo
echo "Logs: journalctl --user -u nesva-agent -f"
echo "Also written to: ${XDG_DATA_HOME:-$HOME/.local/share}/nesva/logs/nesva-agent.log"
