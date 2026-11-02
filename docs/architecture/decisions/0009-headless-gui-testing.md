# 0009. UI compiled as a library and tested headlessly

- Status: Accepted
- Date: 2025-11-02

## Context

The definition of done requires that GUI functionality is actually executed, not assumed. A
primary target is Windows while development happens on Linux, and CI has no desktop session.
Clicking through the app manually is not reproducible evidence.

## Decision

- The Qt Widgets code lives in the static library `nesva_ui`; the `nesva` executable contains
  only `main()`. This lets tests link the real window and the real models.
- `tests/unit/tst_gui_smoke.cpp` constructs the real `AppContext` against a temporary data root,
  seeds a realistic database, instantiates the real `MainWindow`, verifies the eight pages, walks
  every page, and asserts that the Job/Event models read the seeded rows. It runs under
  `QT_QPA_PLATFORM=offscreen` inside CTest.
- The application also has a first-class self-test mode, `nesva --gui-selftest=<dir>`, which
  shows the window, switches through every page, writes one PNG per page and fails with a
  non-zero exit code if any page cannot be captured. It works both offscreen (CI) and on a real
  X11/Wayland session.
- Screenshot evidence is produced by the application itself rather than by an external tool, so
  it cannot drift from the shipped UI code.

## Consequences

- Every CI run proves that the UI constructs, navigates and reads data.
- Automated tests cannot judge visual design; the PNG artifacts are the reviewable evidence.
- `nesva_ui` being a static library slightly increases link time; acceptable.
- A future screenshot-comparison gate can reuse the same harness.

## Alternatives considered

- **Testing only business logic and "manual verification" of the UI**: rejected by the project
  rules, which forbid claiming untested GUI behaviour.
- **Qt Quick Test / Squish-style tooling**: extra dependency and a second test dialect for a
  Widgets application.
