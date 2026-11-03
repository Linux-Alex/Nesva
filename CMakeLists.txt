cmake_minimum_required(VERSION 3.25)

project(Nesva
  VERSION 0.5.0
  DESCRIPTION "Cross-platform desktop backup and archival application"
  HOMEPAGE_URL "https://github.com/nesva-project/nesva"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CTest)
include(NesvaCompilerWarnings)
include(NesvaVersion)

# ---------------------------------------------------------------------------
# Global language settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(NESVA_BUILD_TESTING "Build the Nesva unit and integration tests" ON)
option(NESVA_WARNINGS_AS_ERRORS "Promote compiler warnings to errors" OFF)
option(NESVA_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(NESVA_EMBED_GIT_COMMIT "Embed the current git commit hash in the version header" OFF)

if(NOT NESVA_BUILD_TESTING)
  set(BUILD_TESTING OFF)
endif()

nesva_configure_version_header()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Sql Network Concurrent)

# The Linux credential backend talks to the freedesktop.org Secret Service over
# D-Bus; Windows uses the Credential Manager API instead.
if(UNIX AND NOT APPLE)
  find_package(Qt6 REQUIRED COMPONENTS DBus)
endif()
qt_standard_project_setup(REQUIRES 6.5)

# ---------------------------------------------------------------------------
# nesva-core  (no Qt Widgets, no UI code)
# ---------------------------------------------------------------------------
add_library(nesva_core STATIC
  src/core/app_info.cpp
  src/core/error.cpp
  src/core/external_tool.cpp
  src/core/hashing.cpp
  src/core/logging.cpp
  src/core/paths.cpp
  src/core/process_utils.cpp
  src/core/redactor.cpp
  src/scan/exclusions.cpp
  src/scan/file_scanner.cpp
  src/database/database.cpp
  src/database/migrations.cpp
  src/database/settings_store.cpp
  src/agent/client.cpp
  src/agent/protocol.cpp
  src/backup/restic.cpp
  src/mail/exporter.cpp
  src/mail/graph.cpp
  src/mail/http.cpp
  src/mail/mail_store.cpp
  src/mail/oauth.cpp
  src/security/credential_manager_windows.cpp
  src/security/secret_service_linux.cpp
  src/security/secret_store.cpp
  src/jobs/archive_restore.cpp
  src/jobs/archive_runner.cpp
  src/jobs/backup_runner.cpp
  src/jobs/job_store.cpp
  src/jobs/job_worker.cpp
  include/nesva/jobs/job_worker.hpp
  src/providers/local_provider.cpp
  src/providers/provider_factory.cpp
  src/providers/rclone_provider.cpp
  src/scheduler/schedule.cpp
  src/providers/storage_provider.cpp
)

add_library(Nesva::Core ALIAS nesva_core)

target_include_directories(nesva_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${NESVA_GENERATED_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(nesva_core
  PUBLIC
    Qt6::Core
    Qt6::Sql
    # Qt Network is used for the local agent socket (QLocalSocket/QLocalServer).
    Qt6::Network
)

if(UNIX AND NOT APPLE)
  target_link_libraries(nesva_core PUBLIC Qt6::DBus)
endif()

nesva_set_target_warnings(nesva_core)

# ---------------------------------------------------------------------------
# nesva-desktop  (Qt 6 Widgets application, executable "nesva")
# ---------------------------------------------------------------------------
# The UI lives in a static library so that headless GUI tests can construct the
# real main window in CTest; only main() is part of the executable.
qt_add_library(nesva_ui STATIC
  src/desktop/app_context.hpp
  src/desktop/app_context.cpp
  src/desktop/main_window.hpp
  src/desktop/main_window.cpp
  src/desktop/theme.hpp
  src/desktop/theme.cpp
  src/desktop/icon_factory.hpp
  src/desktop/icon_factory.cpp
  src/desktop/gui_selftest.hpp
  src/desktop/gui_selftest.cpp
  src/desktop/job_executor.hpp
  src/desktop/job_executor.cpp
  src/desktop/dialogs/job_editor_dialog.hpp
  src/desktop/dialogs/job_editor_dialog.cpp
  src/desktop/dialogs/history_dialog.hpp
  src/desktop/dialogs/history_dialog.cpp
  src/desktop/dialogs/snapshot_browser_dialog.hpp
  src/desktop/dialogs/snapshot_browser_dialog.cpp
  src/desktop/dialogs/snapshot_restore_dialog.hpp
  src/desktop/dialogs/snapshot_restore_dialog.cpp
  src/desktop/models/job_list_model.hpp
  src/desktop/models/job_list_model.cpp
  src/desktop/models/event_list_model.hpp
  src/desktop/models/event_list_model.cpp
  src/desktop/pages/page.hpp
  src/desktop/pages/page.cpp
  src/desktop/pages/dashboard_page.hpp
  src/desktop/pages/dashboard_page.cpp
  src/desktop/pages/jobs_page.hpp
  src/desktop/pages/jobs_page.cpp
  src/desktop/pages/storage_page.hpp
  src/desktop/pages/storage_page.cpp
  src/desktop/pages/archive_page.hpp
  src/desktop/pages/archive_page.cpp
  src/desktop/pages/restore_page.hpp
  src/desktop/pages/restore_page.cpp
  src/desktop/pages/snapshots_page.hpp
  src/desktop/pages/snapshots_page.cpp
  src/desktop/pages/activity_page.hpp
  src/desktop/pages/activity_page.cpp
  src/desktop/pages/settings_page.hpp
  src/desktop/pages/settings_page.cpp
  src/desktop/pages/placeholder_page.hpp
  src/desktop/pages/placeholder_page.cpp
)

target_include_directories(nesva_ui PUBLIC ${PROJECT_SOURCE_DIR}/src/desktop)

target_link_libraries(nesva_ui PUBLIC Nesva::Core Qt6::Widgets Qt6::Gui Qt6::Concurrent)

nesva_set_target_warnings(nesva_ui)

qt_add_executable(nesva src/desktop/main.cpp)

target_link_libraries(nesva PRIVATE nesva_ui)

nesva_set_target_warnings(nesva)

set_target_properties(nesva PROPERTIES
  WIN32_EXECUTABLE TRUE
  MACOSX_BUNDLE FALSE
  OUTPUT_NAME "nesva"
)

# ---------------------------------------------------------------------------
# nesva-cli  (automation and diagnostics, executable "nesva-cli")
# ---------------------------------------------------------------------------
add_executable(nesva-cli
  src/cli/main.cpp
  src/cli/command_line.cpp
  src/cli/commands.cpp
)

target_link_libraries(nesva-cli PRIVATE Nesva::Core)

nesva_set_target_warnings(nesva-cli)

set_target_properties(nesva-cli PROPERTIES OUTPUT_NAME "nesva-cli")

# ---------------------------------------------------------------------------
# nesva-agent  (background scheduler and runner, executable "nesva-agent")
# ---------------------------------------------------------------------------
add_executable(nesva-agent
  src/agent/main.cpp
  src/agent/agent_service.hpp
  src/agent/agent_service.cpp
)

target_link_libraries(nesva-agent PRIVATE Nesva::Core Qt6::Network)

nesva_set_target_warnings(nesva-agent)

set_target_properties(nesva-agent PROPERTIES OUTPUT_NAME "nesva-agent")

install(TARGETS nesva-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
install(TARGETS nesva nesva-cli
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS nesva_core
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(UNIX AND NOT APPLE)
  install(FILES packaging/linux/nesva.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES assets/icons/nesva.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    RENAME nesva.svg)
endif()

# ---------------------------------------------------------------------------
# Packaging (CPack). Full installers are produced in the distribution phase.
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "nesva")
set(CPACK_PACKAGE_VENDOR "The Nesva Authors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "security@nesva.invalid")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Nesva")
set(CPACK_STRIP_FILES TRUE)

if(WIN32)
  set(CPACK_GENERATOR "WIX")
elseif(UNIX)
  set(CPACK_GENERATOR "DEB")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "The Nesva Authors <security@nesva.invalid>")
  set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  # Let dpkg-shlibdeps derive the Qt runtime dependencies from the binaries
  # instead of hardcoding distribution-specific package names.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "rclone, restic")
endif()

include(CPack)

message(STATUS "Nesva ${PROJECT_VERSION}: build type=${CMAKE_BUILD_TYPE}, tests=${BUILD_TESTING}")
