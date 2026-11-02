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
