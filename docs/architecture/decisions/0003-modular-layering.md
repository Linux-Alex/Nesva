# 0003. Modular layering: core, UI, CLI, agent

- Status: Accepted
- Date: 2025-11-02

## Context

Nesva needs a GUI, automation (`nesva-cli`) and unattended scheduled execution
(`nesva-agent`). Business logic must not be duplicated or entangled with widgets, and the CLI
must work on machines without a display server.

## Decision

```text
nesva-core (static library; Qt Core + Qt SQL only)
   ▲            ▲                ▲
   │            │                │
nesva_ui     nesva-cli       nesva-agent (Phase 3)
(Widgets)    (no GUI)        (no GUI)
   ▲
nesva (thin main())
```

- `nesva-core` owns every business rule: configuration, database, path handling, logging,
  provider abstraction, job orchestration (Phase 2+).
- `nesva_ui` is a static library so tests can construct the real window headlessly; the
  executable is only `main()`.
- `nesva-cli` links `nesva-core` only, guaranteeing that no widget dependency sneaks in.
- Shared database code is single-instance-per-thread; the GUI uses the instance owned by
  `AppContext`.

## Consequences

- Compile-time enforcement: a `#include <QtWidgets/...>` inside `src/core` fails the CLI build.
- The same logic drives the GUI, the CLI and later the agent, so behaviour cannot diverge.
- Slight extra boilerplate (an internal header directory per component) is accepted.
- Testing the UI requires linking the UI library, which is why GUI tests exist (ADR 0009).

## Alternatives considered

- **Single monolithic executable with `--gui`/`--cli` flags**: hides layering violations and
  bloats the CLI.
- **Dynamic plugins for providers**: premature; a virtual interface plus a factory is enough in
  Phase 2 and keeps deployment simple.
