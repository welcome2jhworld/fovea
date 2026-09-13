find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0>=1.22
  gstreamer-app-1.0
  gstreamer-video-1.0
  gstreamer-pbutils-1.0)
pkg_check_modules(GST_RTSP_SERVER IMPORTED_TARGET gstreamer-rtsp-server-1.0)

# Homebrew's libffi.pc points at a CommandLineTools SDK path that does not
# exist when only Xcode is installed; drop missing include dirs.
function(fovea_prune_missing_includes target)
  if(NOT TARGET ${target})
    return()
  endif()
  get_target_property(_dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
  set(_clean "")
  foreach(_d IN LISTS _dirs)
    if(EXISTS "${_d}")
      list(APPEND _clean "${_d}")
    endif()
  endforeach()
  set_target_properties(${target} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_clean}")
endfunction()
fovea_prune_missing_includes(PkgConfig::GST)
fovea_prune_missing_includes(PkgConfig::GST_RTSP_SERVER)
