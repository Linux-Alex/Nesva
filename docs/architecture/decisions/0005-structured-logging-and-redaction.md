# 0005. Structured JSON logging with mandatory redaction

- Status: Accepted
- Date: 2025-11-02

## Context

Users will file bug reports against cloud transfers and Restic failures. Diagnostics must be
useful without ever leaking tokens, passwords or cookies, and log files must be machine-readable
for support tooling and for the Activity page.

## Decision

- Records are structured (`LogRecord`: UTC timestamp, level, category, message, JSON fields).
- Sinks: a newline-delimited JSON file sink (rotating, size-bounded) and a human console sink.
  Each sink has its own minimum level, so the console stays quiet while the file keeps debug
  detail.
- Categories are dotted (`core.database`, `archive.rclone`, `backup.restic`) so logs can be
  filtered without parsing messages.
- **Redaction is not optional.** `Logger::log()` runs `Redactor` over the message and every field
  (recursively) before any sink sees a record. The redactor censors:
  registered secret values; `password/token/secret/api_key/... = value` assignments; Bearer and
  Basic credentials; PEM private-key blocks; AWS access-key ids; GitHub tokens; JWT-shaped
  values; and any JSON field whose name looks sensitive (`password`, `refresh_token`,
  `client_secret`, ...).
- Rotation keeps `maxBackupFiles` generations and detects a file removed underneath the
  application (external cleanup) and reopens it.
- Logs never contain file contents; only paths, sizes and hashes.

## Consequences

- Logs are greppable as JSON and renderable in a UI table; support can ask for one file.
- A false-positive redaction hides a harmless string - accepted, because a false negative leaks
  a credential.
- Secrets must be registered with `Logger::addSecret()` when they are first read from the
  keyring, otherwise only pattern-based protection applies.
- Sinks are called while the logger holds its mutex; sinks must not log or block on the UI.

## Alternatives considered

- **Plain text logs**: no reliable field extraction, and redaction would be string surgery.
- **Third-party logging library (spdlog/glog)**: adds a dependency and still needs the redaction
  layer; ~350 lines of Qt code cover the requirement.
