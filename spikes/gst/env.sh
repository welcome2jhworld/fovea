# source this before any gst command: the anaconda GStreamer 1.14 on PATH shadows Homebrew 1.26
export PATH=/opt/homebrew/opt/gstreamer/bin:/opt/homebrew/bin:$PATH
export PKG_CONFIG_PATH=/opt/homebrew/opt/gstreamer/lib/pkgconfig:/opt/homebrew/lib/pkgconfig
