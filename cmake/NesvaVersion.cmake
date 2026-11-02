# Generates the nesva/version.hpp header into the build tree.
#
# The header is the single source of truth for the compile-time version of the
# application, and is the file reported by "nesva-cli status" and the About box.

include_guard(GLOBAL)

function(nesva_configure_version_header)
  set(NESVA_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated"
      CACHE INTERNAL "Directory holding generated public headers")

  set(NESVA_VERSION_STRING "${PROJECT_VERSION}")
  set(NESVA_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
  set(NESVA_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
  set(NESVA_VERSION_PATCH "${PROJECT_VERSION_PATCH}")

  if(NESVA_EMBED_GIT_COMMIT)
    execute_process(
      COMMAND git rev-parse --short=12 HEAD
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      OUTPUT_VARIABLE NESVA_GIT_COMMIT
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE git_result
    )
    if(NOT git_result EQUAL 0 OR NOT NESVA_GIT_COMMIT)
      set(NESVA_GIT_COMMIT "unknown")
    endif()
  else()
    set(NESVA_GIT_COMMIT "not-embedded")
  endif()

  file(MAKE_DIRECTORY "${NESVA_GENERATED_INCLUDE_DIR}/nesva")
  configure_file(
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/NesvaVersion.hpp.in"
    "${NESVA_GENERATED_INCLUDE_DIR}/nesva/version.hpp"
    @ONLY
  )

  message(STATUS "Nesva version header: ${NESVA_GENERATED_INCLUDE_DIR}/nesva/version.hpp (${NESVA_VERSION_STRING})")
endfunction()
