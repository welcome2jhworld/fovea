function(fovea_target_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
  endif()
endfunction()

# qt_add_executable builds GUI-subsystem binaries on Windows and bundles on
# macOS; tests and tools need a console so their output reaches CTest.
function(fovea_console_executable target)
  set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
endfunction()
