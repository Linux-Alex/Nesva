# Background agent (`nesva-agent`)

Nesva schedules backups through a small background process, so a job runs even
when the desktop window is closed, and survives a reboot when it is registered
with the operating system.

---

## 1. What the agent does

- Evaluates job schedules on a tick (default 20 s) and starts at most **one** job at a time.
- Catches up a missed slot once when it starts (configurable per job) and skips slots older than
  7 days, so a machine that was off for a month does not start a burst of runs.
- Retries a failed run after a configurable backoff, with the retry state persisted in SQLite so
  a restart neither loses nor duplicates a retry.
- Enforces a maximum run duration (per job) by cancelling the run.
- Serves a local socket so the desktop application and `nesva-cli` can query status, start,
  cancel and reload.
- Writes a heartbeat file (`agent.state.json`) in the data directory, so the UI can show agent
  state even when the socket is not reachable.
- Shuts down gracefully on SIGINT/SIGTERM: the file in flight is completed, the run is closed as
  `cancelled`/`succeeded`, and the heartbeat is marked `stopped`.

Concurrency is still enforced by the database: a partial unique index allows at most one
`running` row per job, so the agent, the desktop application and `nesva-cli` cannot run the same
job twice.

---

## 2. Running it

### Foreground (development)

```bash
./build/linux-debug/nesva-agent --verbose          # tick every 20 s
./build/linux-debug/nesva-agent --tick 5 --verbose # faster tick for testing
./build/linux-debug/nesva-agent --once             # one pass, then exit
```

Options: `--data-dir <path>`, `--tick <seconds>`, `--once`, `--verbose`, `--version`,
`--help`.

### Linux: systemd user service

```bash
scripts/install-agent-service.sh            # installs to ~/.local/bin + ~/.config/systemd/user
systemctl --user status nesva-agent         # check
journalctl --user -u nesva-agent -f         # logs
systemctl --user disable --now nesva-agent  # remove from autostart
```

The unit runs **as the user**, not as root, because scheduled jobs need the user's cloud
credentials and an unlocked keyring. `KillSignal=SIGTERM` (with a 120 s `TimeoutStopSec`) gives
the agent time to finish the current file.

### Windows: per-user Run entry

```powershell
nesva-cli agent install      # writes HKCU\Software\Microsoft\Windows\CurrentVersion\Run
nesva-cli agent status
nesva-cli agent uninstall
```

The entry starts `nesva-agent.exe` at logon in the user's interactive session, without
PowerShell, a scheduled task or elevation. The agent has no window or tray icon. The desktop
window's optional sign-in entry is a separate `Nesva` Run value managed under
**Settings -> Window behaviour**.
**Windows SYSTEM does not have access to an interactive user's cloud credentials or keyring**, so
a system-wide service is deliberately not provided.

---

## 3. Controlling the agent

```bash
nesva-cli agent status            # exit 0 when running, 3 when stopped
nesva-cli --json agent status     # machine-readable
nesva-cli agent run 3             # ask the agent to run job 3 now
nesva-cli agent cancel 3          # cancel the running job 3
nesva-cli agent reload            # re-read schedules (also SIGHUP)
nesva-cli agent stop              # graceful shutdown
nesva-cli schedule list           # schedules with their next slot
```

The desktop application uses the same socket: "Run now" is delegated to the agent when it is
reachable, and falls back to an in-process run otherwise. The Settings page shows agent state and
can stop it.

---

## 4. IPC protocol

One JSON object per line over a `QLocalServer` socket named
`nesva-agent-<first 16 hex of sha1(data-root)>` (a Unix socket in `$TMPDIR` on Linux, a named
pipe on Windows). The hash keeps separate data directories isolated, which matters for tests and
for portable installs.

Request: `{"command":"run","job_id":3,"dry_run":false,"trigger":"cli"}`

| Command | Fields | Response |
|---|---|---|
| `status` | - | `{"ok":true,"running":true,"state":"idle","pid":…,"scheduled_jobs":…,"next_run_at":…}` |
| `run` | `job_id` | `{"ok":true,"started":3}` |
| `cancel` | `job_id` | `{"ok":true,"cancelling":3}` |
| `reload` | - | `{"ok":true,"reloaded":true}` |
| `shutdown` | - | `{"ok":true,"stopping":true}` |

Failures: `{"ok":false,"error":{"code":"not_found","message":"…"}}`.

---

## 5. State and files

| Path | Purpose |
|---|---|
| `<data>/agent.lock` | single-instance lock (`QLockFile`) |
| `<data>/agent.state.json` | heartbeat: pid, state, running job, next run, last message |
| `<data>/logs/nesva-agent.log` | structured JSON log (separate from the desktop log) |
| `job_schedules` table | per-job schedule, next slot, retry state |
| `job_runs` table | every run with `owner_pid`/`owner_host` for crash recovery |

A run whose owning process disappeared is marked `interrupted` at the next start (by the agent
or the desktop application), so a crash never blocks a job permanently.

---

## 6. Troubleshooting

| Symptom | Cause |
|---|---|
| `nesva-cli agent status` exits 3 | agent not running: start it, or check `systemctl --user status nesva-agent` |
| `another nesva-agent is already running` | the lock file exists; stop the other process (or remove a stale lock after verifying no agent runs) |
| Job never runs | job disabled, schedule is "Manual only", or the slot is older than the catch-up window |
| Run says `cancelled` | maximum duration reached, or `agent stop` was requested during the run |
| Notifications do not appear | notifications are desktop-side; the agent itself never pops dialogs |

---

## 7. Limits of this phase

- The agent runs archive jobs only; Restic backup jobs arrive in Phase 4.
- `on_login` and `on_startup` are equivalent for a user-session agent: both run once per agent
  start.
- Network-availability checks before a run are not implemented yet.
