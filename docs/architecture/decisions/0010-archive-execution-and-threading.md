# 0010. Archive execution on a worker thread with database-enforced single flight

- Status: Accepted
- Date: 2026-11-02

## Context

Phase 2 must transfer files without freezing the window, must never run the same job twice
concurrently, must survive a crash mid-run, and must keep the UI readable while a transfer writes
history.

Qt SQL connections are thread-affine: a `QSqlDatabase` opened on the GUI thread must not be used
from another thread. Cancellation must still reach a worker that is blocked inside a transfer.

## Decision

**Execution model**

- `JobExecutor` (GUI thread) owns a `QThread`-derived worker per run.
- The worker opens its **own** `db::Database` connection (same file, WAL mode), so the GUI thread
  keeps reading while the worker writes.
- The worker runs `jobs::ArchiveRunner`, which is a plain class: no Qt event loop dependency, so
  it can also be driven synchronously by `nesva-cli`.
- Progress is emitted from the worker and delivered to the GUI through queued signals
  (`qRegisterMetaType<RunProgress>()`).

**Cancellation**

- A `CancellationToken` holds a `std::shared_ptr<std::atomic<bool>>`; the GUI calls `cancel()`
  directly on the worker object, without a queued call, because the worker's event loop is
  blocked for the duration of the run.
- The runner checks the token between files and inside copy loops and returns
  `ErrorCode::Cancelled`, never a partial "success".

**Single flight and crash recovery**

- A partial unique index in the schema makes a second `'running'` row for the same job
  impossible: `CREATE UNIQUE INDEX ux_job_runs_single_running ON job_runs(job_id)
  WHERE status = 'running'`. The UI cannot bypass this, and neither can the CLI or a future
  agent.
- Each run records `owner_pid` and `owner_host`. At startup the application marks runs whose
  owner process is gone (same host, PID not alive) as `'interrupted'`. Runs owned by another
  host are left untouched because their liveness cannot be verified here.

**Restore**

- Restore runs through `QtConcurrent::run` with its own database connection; progress is
  marshalled back with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.

## Consequences

- The window never blocks on a transfer; the Dashboard and Jobs pages update live.
- A killed process cannot leave a job permanently "running".
- Two Nesva instances cannot corrupt each other's run history, though only one GUI instance is
  started per user in practice.
- The runner cannot rely on Qt's event loop, which keeps it unit-testable headlessly (12 tests in
  `tst_archive_runner` plus worker-thread tests in `tst_gui_smoke`).

## Alternatives considered

- **`QThreadPool` / `QtConcurrent` for archive runs**: no place to expose cancellation or a
  per-run provider instance; a dedicated thread object is simpler to reason about.
- **Single connection shared across threads**: violates Qt's connection affinity and risks
  corruption.
- **In-memory "is running" flag only**: would not survive a crash or a second process, which is
  exactly the failure mode a backup tool must not have.
