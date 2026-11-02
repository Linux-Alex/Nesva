# Architecture decision records

Each decision that shapes the code base is recorded here as a short ADR: context, decision,
consequences and alternatives considered. ADRs are immutable once accepted; a later decision
supersedes an earlier one instead of editing it.

| ADR | Title | Status |
|---|---|---|
| [0001](0001-cpp20-qt6-widgets.md) | C++20 and Qt 6 Widgets for the desktop UI | Accepted |
| [0002](0002-cmake-presets-and-ninja.md) | CMake with presets and Ninja as the build system | Accepted |
| [0003](0003-modular-layering.md) | Modular layering: core, UI, CLI, agent | Accepted |
| [0004](0004-sqlite-schema-and-migrations.md) | SQLite with forward-only migrations | Accepted |
| [0005](0005-structured-logging-and-redaction.md) | Structured JSON logging with mandatory redaction | Accepted |
| [0006](0006-external-tool-integration.md) | Rclone and Restic as external tools invoked without a shell | Accepted |
| [0007](0007-backup-safety-copy-semantics.md) | Copy semantics and non-destructive defaults | Accepted |
| [0008](0008-secrets-and-credential-references.md) | Secrets live in the OS keyring; SQLite stores references | Accepted |
| [0009](0009-headless-gui-testing.md) | UI compiled as a library and tested headlessly | Accepted |
| [0010](0010-archive-execution-and-threading.md) | Archive execution on a worker thread with database-enforced single flight | Accepted |
| [0011](0011-background-agent-and-scheduling.md) | Background agent, schedule evaluation and local IPC | Accepted |

## Template

```markdown
# NNNN. Title

- Status: Proposed | Accepted | Superseded by NNNN
- Date: YYYY-MM-DD
- Deciders: ...

## Context

What problem forces a decision?

## Decision

What was decided, concretely.

## Consequences

Positive, negative, and follow-up work.

## Alternatives considered

What was rejected, and why.
```
