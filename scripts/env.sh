#!/bin/sh
# Source this before building or running on macOS: fixes PATH (anaconda ships an
# old GStreamer 1.14 that shadows Homebrew) and pkg-config lookup.
export GST_ROOT="${GST_ROOT:-/opt/homebrew/opt/gstreamer}"
export QT_ROOT="${QT_ROOT:-/opt/homebrew/opt/qt}"
export PATH="$GST_ROOT/bin:$QT_ROOT/bin:/opt/homebrew/bin:$PATH"
export PKG_CONFIG_PATH="$GST_ROOT/lib/pkgconfig:/opt/homebrew/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export GST_PLUGIN_SYSTEM_PATH_1_0="$GST_ROOT/lib/gstreamer-1.0"
export CMAKE_PREFIX_PATH="$QT_ROOT${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
