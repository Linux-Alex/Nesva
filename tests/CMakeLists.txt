# Test suite. Every test is a Qt Test binary registered with CTest.

find_package(Qt6 REQUIRED COMPONENTS Test)

add_library(nesva_test_support STATIC
  support/test_paths.hpp
)

target_link_libraries(nesva_test_support PUBLIC Nesva::Core Qt6::Test)
target_include_directories(nesva_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

function(nesva_add_unit_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE nesva_test_support)
  nesva_set_target_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 120
  )
endfunction()

nesva_add_unit_test(tst_result unit/tst_result.cpp)
nesva_add_unit_test(tst_redactor unit/tst_redactor.cpp)
nesva_add_unit_test(tst_logging unit/tst_logging.cpp)
nesva_add_unit_test(tst_paths unit/tst_paths.cpp)
nesva_add_unit_test(tst_database unit/tst_database.cpp)
nesva_add_unit_test(tst_settings_store unit/tst_settings_store.cpp)
nesva_add_unit_test(tst_external_tool unit/tst_external_tool.cpp)

# GUI smoke test: constructs the real main window and models headlessly.
nesva_add_unit_test(tst_gui_smoke unit/tst_gui_smoke.cpp)
target_link_libraries(tst_gui_smoke PRIVATE nesva_ui)

# Future integration tests live in tests/integration and are added here as the
# corresponding features land (local archive, restore, cancellation, ...).
