# 0002. CMake with presets and Ninja as the build system

- Status: Accepted
- Date: 2025-11-02

## Context

The project must build from the command line on Linux (GCC and Clang) and Windows (MSVC) without
requiring Qt Creator, with reproducible configurations, CTest integration, static analysis and
packaging targets.

## Decision

- Modern CMake (≥ 3.25, target-based, no globbing of sources) with `CMakePresets.json`.
- Ninja as the generator for all developer presets.
- Preset names are the single source of truth for how the project is configured:
  `linux-debug`, `linux-release`, `linux-clang-debug`, `linux-asan`, `windows-msvc-debug`,
  `windows-msvc-release`, plus matching build and test presets.
- Warnings are centralised in `cmake/NesvaCompilerWarnings.cmake`
  (`-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align -Wunused
  -Woverloaded-virtual -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wnull-dereference
  -Wextra-semi` for GCC/Clang, `/W4 /permissive- /Zc:__cplusplus` for MSVC) and promoted to
  errors in the presets used by CI.
- The version header is generated at configure time into the build tree
  (`build/<preset>/generated/nesva/version.hpp`), so no generated file is committed.

## Consequences

- One command per step: `cmake --preset linux-debug`, `cmake --build --preset linux-debug`,
  `ctest --preset linux-debug --output-on-failure`.
- Adding a source file requires editing `CMakeLists.txt` - deliberate, so the file list stays
  auditable and reproducible.
- Windows MSVC presets must be launched from a Visual Studio developer prompt (documented).
- Warnings-as-errors keeps the tree clean but requires a clean build after compiler upgrades.

## Alternatives considered

- **Meson**: good generator story, weaker Qt/CPack integration and less common in Qt shops.
- **qmake**: single-platform legacy, no preset/CTest ecosystem.
- **Makefiles only**: no generator portability, no reliable parallel builds on Windows.
