cmake_minimum_required(VERSION 3.20)
project(fovea_gst_spike CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0)

# Homebrew libffi.pc advertises an SDK include dir that does not exist on this machine;
# drop missing dirs so CMake does not reject the target.
set(GST_INCLUDES "")
foreach(dir IN LISTS GST_INCLUDE_DIRS)
    if(EXISTS "${dir}")
        list(APPEND GST_INCLUDES "${dir}")
    endif()
endforeach()

foreach(exe rtsp_server pipe_probe)
    add_executable(${exe} ${exe}.cpp)
    target_include_directories(${exe} PRIVATE ${GST_INCLUDES})
    target_link_directories(${exe} PRIVATE ${GST_LIBRARY_DIRS})
    target_link_libraries(${exe} PRIVATE ${GST_LIBRARIES})
    target_compile_options(${exe} PRIVATE ${GST_CFLAGS_OTHER})
endforeach()
