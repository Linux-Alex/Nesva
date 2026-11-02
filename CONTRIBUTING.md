# Contributing to Nesva

Thanks for helping. This project values correctness over speed, and safety over convenience.

## Ground rules

1. **Never delete or overwrite user data.** Archive mode is copy-only. Any destructive behaviour
   requires an explicit setting, a confirmation and tests.
2. **No secrets in the repository, logs or error messages.** Credentials go to the OS keyring;
   everything that reaches a log sink passes through the redactor.
3. **No custom cryptography.** Encryption is Restic's job.
4. **No shell invocation** for external tools - pass argument arrays to `QProcess`.
5. Keep documentation, ADRs and code in sync in the same change.

## Workflow

```bash
git switch -c feature/short-description
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug --output-on-failure
```

A change is ready when:

- it compiles with warnings-as-errors (`NESVA_WARNINGS_AS_ERRORS=ON`, used by the presets and CI);
- new behaviour has unit tests, and error paths are covered;
- GUI-affecting changes keep `tst_gui_smoke` and `--gui-selftest` green;
- relevant docs (`docs/`) and, for architectural changes, a new ADR are added;
- no unrelated files are touched.

## Style

- C++20, 4-space indentation, 100-column lines, `.clang-format` is authoritative.
- Prefer small, testable modules; return `Result<T>`/`Status` for expected failures instead of
  throwing.
- Business logic belongs in `src/core` or `src/database` and must not include Qt Widgets.
- UI code lives in `src/desktop`; models own SQL, views do not.
- Comments explain *why*; naming explains *what*. No commented-out code.

## Tests

- Unit tests live in `tests/unit` as Qt Test suites registered with CTest via
  `nesva_add_unit_test(...)` in `tests/CMakeLists.txt`.
- Use `QTemporaryDir` and the `ScopedDataRoot` helper; never point tests at a real data
  directory.
- Cloud integration tests must be opt-in, use a dedicated account and an isolated remote prefix.

## Commits and pull requests

- One logical change per commit; imperative subject line ("Add restore conflict policy").
- Describe *what* and *why* in the body, and note any platform limitation introduced.
- Reference the phase or ADR affected.
- Pull requests must state which platforms were actually tested. Do not claim Windows testing
  from Linux.

## Reporting bugs

Include: `nesva-cli doctor` output, the relevant log excerpt (redacted automatically), the exact
command, and what you expected. Security issues go to `SECURITY.md`, not the public tracker.
