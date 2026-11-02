# 0011. Background agent, schedule evaluation and local IPC

- Status: Accepted
- Date: 2026-11-02

## Context

Phase 3 requires scheduled runs that survive the GUI closing and a reboot, explicit user-context
execution, graceful shutdown, IPC with the desktop application, persistent execution state and
protection against duplicate concurrent runs. An in-process timer is explicitly not acceptable.

## Decision

**Separate process, same core**

- `nesva-agent` is a `QCoreApplication` executable linking `nesva-core` (no widgets).
- It opens its own SQLite connection, applies migrations, recovers runs whose owner process
  disappeared, then loops.
- A `QLockFile` (`<data>/agent.lock`) guarantees a single agent per data directory; the socket is
  additionally namespaced by a hash of the data root so separate installations never collide.

**Schedule evaluation is a pure function**

- `sched::computeNextRun()` and `sched::evaluateDue()` contain all the calendar logic (daily,
  weekly, monthly, timezone-aware, DST-gap safe), and are unit-tested without a database or a
  clock.
- Missed slots are caught up **once** when the agent starts, and slots older than a configurable
  window (default 7 days) are skipped, so a machine that was off for a month does not start a
  burst.
- Retries use geometric backoff and are persisted in `job_schedules.retry_attempt_count` /
  `retry_next_at` so a restart neither loses nor duplicates a retry.
- `on_login` and `on_startup` run once per agent session.

**Execution is still the archive runner**

- The agent reuses `jobs::JobWorker` (the same worker the desktop uses) and `jobs::ArchiveRunner`,
  so a scheduled run and a manual run cannot drift apart in behaviour.
- One job at a time, plus the database's partial unique index for `status = 'running'`, so the
  agent, the desktop and the CLI cannot double-run a job.
- A per-job maximum duration is enforced by cancelling the worker.

**IPC is a line-oriented JSON protocol over a local socket**

- `QLocalServer`/`QLocalSocket` (Unix socket on Linux, named pipe on Windows), one JSON object
  per line: `status`, `run`, `cancel`, `reload`, `shutdown`.
- Errors are structured: `{"ok":false,"error":{"code":"not_found","message":…}}`.
- The accept handler **drains already-buffered data** before relying on `readyRead`, because a
  local client writes immediately and `readyRead` only fires for new data. This was a real bug
  found by the integration test.
- A heartbeat file (`<data>/agent.state.json`) is written on every tick and every state change, so
  the UI can show agent state without a socket round-trip and the agent's state is inspectable
  from the filesystem.

**Notifications and tray stay in the desktop application**

- The agent is headless by design (it also runs on a server with no session). Notifications and
  the tray icon are the window's job; the agent only reports state.
- Closing the window keeps it in the tray only when the user enables it and no in-process run is
  active; scheduled execution is unaffected either way.

## Consequences

- Scheduled runs keep working with no window, and after a reboot when the systemd user unit or
  the scheduled task is registered.
- `nesva-cli agent status|run|cancel|reload|stop` and the desktop share one client class, so they
  behave identically.
- The agent needs an unlocked keyring for cloud credentials; documented in
  `docs/developer-guide/agent.md`.
- Windows has no equivalent of a systemd user unit here: a per-user scheduled task with
  `LogonType Interactive` and `RunLevel Limited` is registered by a PowerShell script. A
  system-wide SYSTEM service is deliberately **not** offered, because SYSTEM cannot see a user's
  cloud credentials.

## Alternatives considered

- **In-process GUI timer**: explicitly rejected by the requirements; dies with the window.
- **A database-backed job queue polled by the desktop**: still dies with the window.
- **TCP on localhost instead of a local socket**: exposes the control channel to the network;
  a per-user Unix socket / named pipe is simpler and safer.
- **HTTP/REST with a small web server**: more code, more attack surface, no benefit for a local,
  single-user control channel.
