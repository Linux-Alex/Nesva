# Compiler warning configuration shared by every Nesva target.
#
# Usage: nesva_set_target_warnings(<target>)

function(nesva_set_target_warnings target)
  set(gcc_clang_warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wnull-dereference
    -Wextra-semi
  )

  set(msvc_warnings
    /W4
    /permissive-
    /Zc:__cplusplus
    /Zc:inline
    /utf-8
  )

  # GCC 13's -Wnull-dereference is known to report false positives at -O3 in
  # heavily inlined code - especially Qt's queued-connection functor chains,
  # where it concludes that 'this' itself may be null. GCC 14 fixed most of
  # them, so the check stays enabled there and only the affected versions skip
  # it. Verified on GCC 13.1 (MinGW) and GCC 15 (Linux): without this, a
  # perfectly valid 'if (member_ == nullptr)' is reported as a dereference.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
     AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    list(REMOVE_ITEM gcc_clang_warnings -Wnull-dereference)
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE ${msvc_warnings})
    if(NESVA_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE ${gcc_clang_warnings})
    if(NESVA_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(NESVA_ENABLE_SANITIZERS)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()
