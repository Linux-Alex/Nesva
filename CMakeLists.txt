cmake_minimum_required(VERSION 3.25)

project(Nesva
  VERSION 0.6.6
  DESCRIPTION "Cross-platform desktop backup and archival application"
  HOMEPAGE_URL "https://github.com/Linux-Alex/Nesva"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CTest)

if(UNIX AND NOT APPLE)
  # Nesva's private application directory. Every file Nesva installs on Linux
  # lives under it, and nothing there is read by any other application.
  #
  # This is not a style choice: qt.conf is resolved relative to the executable, so
  # a qt.conf in a shared directory such as /usr/bin applies to *every* Qt program
  # started from there. Nesva 0.6.4 shipped exactly that, and it made KDE Spectacle
  # and other Qt applications look for plugins in /usr/lib/nesva/plugins.
  set(NESVA_APP_DIR "lib/nesva")
  set(NESVA_APP_BINDIR "${NESVA_APP_DIR}/bin")
  set(NESVA_APP_LIBDIR "${NESVA_APP_DIR}/lib")
  set(NESVA_APP_PLUGINDIR "${NESVA_APP_DIR}/plugins")
  # /usr/bin holds only symlinks into the private directory (see the install rules).
  set(NESVA_LAUNCHER_DIR "${CMAKE_INSTALL_BINDIR}")

  # Installed binaries find the Qt runtime that cmake/DeployQtRuntime.cmake.in
  # copies next to them, so the packages never depend on a Qt the distribution
  # does not ship. Set at link time rather than with patchelf: one less build
  # dependency.
  #
  # DT_RPATH, not the default DT_RUNPATH: the loader searches RUNPATH *after*
  # LD_LIBRARY_PATH, so a user with an older Qt on their LD_LIBRARY_PATH would
  # silently load it against these binaries. RPATH is searched first and the
  # bundled Qt therefore always wins.
  set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
  add_link_options(-Wl,--disable-new-dtags)
endif()
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

# The RC language compiles the Windows icon/version resource below. It is only
# needed on Windows, where CMake does not enable it through LANGUAGES CXX.
if(WIN32)
  enable_language(RC)
endif()

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
  src/core/disk_space.cpp
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
  src/agent/autostart.cpp
  src/agent/client.cpp
  src/agent/protocol.cpp
  src/backup/restic.cpp
  src/mail/exporter.cpp
  src/mail/graph.cpp
  src/core/http.cpp
  src/core/updater.cpp
  src/mail/mail_store.cpp
  src/mail/oauth.cpp
  src/mail/session.cpp
  # One platform backend per build: compiling the other one is wasted work at
  # best (both files guard their bodies, but MSVC still parses them).
  $<$<PLATFORM_ID:Windows>:src/security/credential_manager_windows.cpp>
  $<$<PLATFORM_ID:Linux>:src/security/secret_service_linux.cpp>
  src/security/identity.cpp
  src/security/secret_store.cpp
  src/jobs/archive_restore.cpp
  src/jobs/archive_runner.cpp
  src/jobs/backup_runner.cpp
  src/jobs/job_store.cpp
  src/jobs/job_worker.cpp
  src/jobs/mail_job_runner.cpp
  include/nesva/jobs/job_worker.hpp
  src/tools/tool_installer.cpp
  src/tools/tool_launcher.cpp
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
  src/desktop/app_autostart.hpp
  src/desktop/app_autostart.cpp
  src/desktop/single_instance.hpp
  src/desktop/single_instance.cpp
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
  src/desktop/dialogs/rclone_config_dialog.hpp
  src/desktop/dialogs/rclone_config_dialog.cpp
  src/desktop/dialogs/snapshot_restore_dialog.hpp
  src/desktop/dialogs/snapshot_restore_dialog.cpp
  src/desktop/dialogs/entra_help_dialog.hpp
  src/desktop/dialogs/entra_help_dialog.cpp
  src/desktop/dialogs/mail_connect_dialog.hpp
  src/desktop/dialogs/mail_connect_dialog.cpp
  src/desktop/dialogs/tool_install_dialog.hpp
  src/desktop/dialogs/tool_install_dialog.cpp
  src/desktop/widgets/elided_label.hpp
  src/desktop/widgets/flow_layout.hpp
  src/desktop/widgets/flow_layout.cpp
  src/desktop/time_display.hpp
  src/desktop/time_display.cpp
  src/desktop/widgets/elided_label.cpp
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
  src/desktop/pages/mail_page.cpp
  src/desktop/pages/settings_page.cpp
  src/desktop/pages/placeholder_page.hpp
  src/desktop/pages/placeholder_page.cpp
)

target_include_directories(nesva_ui PUBLIC ${PROJECT_SOURCE_DIR}/src/desktop)

target_link_libraries(nesva_ui PUBLIC Nesva::Core Qt6::Widgets Qt6::Gui Qt6::Concurrent)

nesva_set_target_warnings(nesva_ui)

# The application icon is a Windows resource: QApplication::setWindowIcon()
# only affects the windows at runtime, while Explorer, shortcuts, the taskbar
# and the NSIS installer read the icon embedded in the executable. The same
# resource carries the version information shown in File Properties.
if(WIN32)
  set(NESVA_ICON_PATH "${PROJECT_SOURCE_DIR}/assets/icons/nesva.ico")
  set(NESVA_VERSION_COMMA
      "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
  set(NESVA_WINDOWS_RC "${CMAKE_CURRENT_BINARY_DIR}/nesva.rc")
  configure_file(packaging/windows/nesva.rc.in "${NESVA_WINDOWS_RC}" @ONLY)
  qt_add_executable(nesva src/desktop/main.cpp "${NESVA_WINDOWS_RC}")
else()
  qt_add_executable(nesva src/desktop/main.cpp)
endif()

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
# qt_add_executable, not add_executable: with WIN32_EXECUTABLE below the target
# needs Qt's WinMain entry point (Qt6::EntryPointPrivate) to call main(), and
# qt_add_executable links it the same way it already does for the desktop binary.
qt_add_executable(nesva-agent
  src/agent/main.cpp
  src/agent/agent_service.hpp
  src/agent/agent_service.cpp
)

target_link_libraries(nesva-agent PRIVATE Nesva::Core Qt6::Network)

nesva_set_target_warnings(nesva-agent)

set_target_properties(nesva-agent PROPERTIES OUTPUT_NAME "nesva-agent")

if(WIN32)
  # Started by the logon Run entry, a console-subsystem binary makes Windows open
  # a console window in the user's session every time they log in. The agent is a
  # background helper: no window, log file only. Started from a terminal it attaches
  # to that console instead (see src/agent/main.cpp), so --help and --verbose still
  # print where the user can see them.
  set_target_properties(nesva-agent PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
  # The executables, the bundled Qt runtime and its plugins all live in the
  # private directory; /usr/bin only ever holds symlinks into it.
  install(TARGETS nesva nesva-cli nesva-agent
    RUNTIME DESTINATION ${NESVA_APP_BINDIR}
  )

  # Relative symlinks, so the same install tree works under /usr for the .deb and
  # under any prefix for the portable tarball. Generated rather than inlined: an
  # install(CODE) string expands its ${} at configure time, which silently produced
  # links in / instead of the install tree.
  configure_file(cmake/CreateLauncherLinks.cmake.in
                 "${CMAKE_CURRENT_BINARY_DIR}/CreateLauncherLinks.cmake" @ONLY)
  install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/CreateLauncherLinks.cmake")

  install(FILES packaging/linux/nesva.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES assets/icons/nesva.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    RENAME nesva.svg)
else()
  install(TARGETS nesva nesva-cli nesva-agent
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
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
  # The Windows installer is packaging/windows/nesva.nsi, built by the Release
  # workflow from the staged directory that already contains the deployed Qt
  # runtime: CPack's own NSIS generator would stage a tree without it. CPack here
  # produces only the portable zip.
  set(CPACK_GENERATOR "ZIP")
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
elseif(UNIX)
  # DEB plus a portable archive: both are produced from the same staged tree, so
  # the Qt bundling below cannot apply to one and not the other.
  set(CPACK_GENERATOR "DEB;TGZ")
  set(CPACK_PACKAGE_FILE_NAME "nesva-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
  # Bundle the Qt runtime into the package: the release Qt is newer than anything a
  # distribution ships, so a package that depended on the system Qt would install
  # and then fail to start. Qt's own deploy script also copies every system library
  # Qt touches (226 MB of X11, cups, avahi, yuv), so this does the narrower thing:
  # the Qt libraries the binaries need plus the plugin families Nesva loads, while
  # everything Qt itself needs stays a package dependency.
  # A packager can point at the Qt installation explicitly; otherwise the one this
  # build links against is used.
  if(DEFINED NESVA_QT_RUNTIME_DIR AND NESVA_QT_RUNTIME_DIR)
    message(STATUS "Using the Qt runtime from ${NESVA_QT_RUNTIME_DIR}")
  elseif(DEFINED QT6_INSTALL_PREFIX)
    set(NESVA_QT_RUNTIME_DIR "${QT6_INSTALL_PREFIX}")
  elseif(DEFINED QT_INSTALL_PREFIX)
    set(NESVA_QT_RUNTIME_DIR "${QT_INSTALL_PREFIX}")
  else()
    get_target_property(nesva_qt_core_file Qt6::Core IMPORTED_LOCATION)
    if(nesva_qt_core_file)
      get_filename_component(NESVA_QT_RUNTIME_DIR "${nesva_qt_core_file}" DIRECTORY)
      get_filename_component(NESVA_QT_RUNTIME_DIR "${NESVA_QT_RUNTIME_DIR}" DIRECTORY)
    endif()
  endif()
  configure_file(cmake/DeployQtRuntime.cmake.in
                 "${CMAKE_CURRENT_BINARY_DIR}/DeployQtRuntime.cmake" @ONLY)
  install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/DeployQtRuntime.cmake")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "The Nesva Authors <security@nesva.invalid>")
  set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
  # Maintainer script: removes the global /usr/bin/qt.conf that 0.6.4 and earlier
  # shipped, but only when its contents are byte-for-byte Nesva's and no package
  # owns the path. See packaging/linux/debian/postinst.
  set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/debian/postinst")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  # Let dpkg-shlibdeps derive the Qt runtime dependencies from the binaries
  # instead of hardcoding distribution-specific package names.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "rclone, restic")
endif()

include(CPack)

message(STATUS "Nesva ${PROJECT_VERSION}: build type=${CMAKE_BUILD_TYPE}, tests=${BUILD_TESTING}")
