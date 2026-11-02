# 0001. C++20 and Qt 6 Widgets for the desktop UI

- Status: Accepted
- Date: 2025-11-02

## Context

Nesva is a desktop backup and archival application for Windows 10/11 (primary) and Linux
(primary development, secondary target). The UI must feel like a professional administration
tool, support light/dark themes, high-DPI, keyboard navigation and native dialogs, and stay
responsive during long transfers. The product must be maintainable by C++ developers and ships
without a browser runtime.

## Decision

- Language: **C++20** (concepts, `std::variant`, `std::optional`, `std::string_view`,
  designated initialisers, `<bit>` where needed).
- UI toolkit: **Qt 6 Widgets**, not QML, for the initial implementation.
- No Electron, Tauri, React, or embedded browser UI.

Widgets were chosen over QML because table-heavy administrative interfaces, Model/View, native
menus, system tray and system dialogs are first-class in Widgets, and because the team's
C++/Qt expertise maps directly onto Widgets.

## Consequences

- The application is a normal native binary; packaging ships the Qt runtime (deployed with
  `windeployqt` on Windows).
- Model/View (`QAbstractTableModel`) gives filtering, sorting and virtualisation for free.
- Styling is done via palettes and a small stylesheet instead of QML declarative styling; no
  animations are added deliberately.
- Memory and startup cost stay low compared with a browser-based stack.
- A future QML front end remains possible because business logic lives in `nesva-core`, which
  does not depend on Qt Widgets at all.

## Alternatives considered

- **QML/Qt Quick**: rejected for the administrative, table-centric UI and for the added
  declarative/Debug cycle; the requirement explicitly rules out web-style UI frameworks.
- **GTK/wxWidgets**: rejected - weaker cross-platform packaging story and less consistent
  Windows look, and the project mandates Qt.
- **Electron/Tauri**: rejected by the product requirements (no embedded browser UI).
