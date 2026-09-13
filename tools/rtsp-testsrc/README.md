# rtsp-testsrc

Local RTSP H.264 source for Fovea integration tests. Serves a `videotestsrc`
pattern, a capture device (webcam) or an H.264 MP4/MOV file on `127.0.0.1`
only, over RTP/UDP or RTP-over-RTSP (TCP interleaved), with the same
media-factory behaviour a camera would present (shared media, SPS/PPS in-band,
`pt=96`).

## Build

Requires GStreamer 1.20+ with `gst-rtsp-server`, plus the `x264`,
`libav`, `rtp`, `videotestsrc`, `pango` (overlays) and `isomp4` plugins.
Webcam mode additionally needs `applemedia` on macOS (`avfvideosrc`,
`vtenc_h264`), `video4linux2` on Linux or `winks` on Windows, and
`videoscale`/`videorate`.

```sh
export PATH=/opt/homebrew/opt/gstreamer/bin:/opt/homebrew/bin:$PATH
export PKG_CONFIG_PATH=/opt/homebrew/opt/gstreamer/lib/pkgconfig:/opt/homebrew/lib/pkgconfig
cmake -S tools/rtsp-testsrc -B build/rtsp-testsrc -G Ninja
ninja -C build/rtsp-testsrc
```

From a parent project: `add_subdirectory(tools/rtsp-testsrc)` and use
`$<TARGET_FILE:fovea-rtsp-testsrc>` (binary name `rtsp-testsrc`). If the
parent already defines `PkgConfig::GST` and `PkgConfig::GST_RTSP_SERVER`
(as the Fovea tree does) they are linked as-is; otherwise the tool runs its own
`pkg_check_modules` under the `GSTRTSP` prefix, so it never collides with a
parent's `GST` variables. `pkg-config` must then be able to find
`gstreamer-1.0` and `gstreamer-rtsp-server-1.0`; on Homebrew pass
`PKG_CONFIG_PATH` as above or `-DCMAKE_PREFIX_PATH=/opt/homebrew/opt/gstreamer`.
In the Fovea tree the target is built with `cmake --preset macos-dev
-DFOVEA_BUILD_TOOLS=ON && cmake --build --preset macos-dev --target
fovea-rtsp-testsrc` and lands at `build/macos-dev/tools/rtsp-testsrc/rtsp-testsrc`.

## Usage

```
rtsp-testsrc [--port 8554] [--path /test]
             [--pattern ball|smpte|snow|...] [--width W] [--height H] [--fps 25]
             [--webcam [--device-index 0]] [--list-devices]
             [--file clip.mp4] [--loop] [--overlay | --no-overlay]
             [--keyint 25] [--bitrate-kbps 800]
             [--stop-after SEC]
```

The first stdout line is the URL, e.g. `rtsp://127.0.0.1:8554/test`, printed
after the port is bound (`--port 0` picks a free port). The second line is
the launch description. `SIGINT`/`SIGTERM` shut the server down cleanly;
`--stop-after N` exits after N seconds so a test can simulate a camera going
away (connected clients see the connection drop and report EOS/error).

Pattern mode (default, 640x360 unless `--width/--height` are given) encodes
live with
`x264enc tune=zerolatency speed-preset=ultrafast bframes=0 key-int-max=<keyint> bitrate=<kbps>`
and burns two overlays into the frame: `timeoverlay` (stream running time,
top-left) and a wall clock with milliseconds (top-right, `HH:MM:SS.mmm`, local
time, stamped when the frame passes the overlay). Compare the wall clock in a
received frame with the receive time to measure glass-to-glass latency.
`clockoverlay` is not used because its `strftime` format is limited to whole
seconds.

File mode (`--file`) sends the H.264 track as-is:
`filesrc ! qtdemux ! h264parse ! rtph264pay pt=96 config-interval=1`.
`--width/--height/--fps/--bitrate-kbps` are ignored unless `--overlay` is
given, which decodes, burns the overlays, and re-encodes with the x264
settings. Only MP4/MOV containers are supported (`qtdemux`).

Clients:

```sh
gst-launch-1.0 rtspsrc location=rtsp://127.0.0.1:8554/test protocols=tcp latency=500 \
  ! rtph264depay ! h264parse ! avdec_h264 ! fakesink sync=true
gst-launch-1.0 rtspsrc location=rtsp://127.0.0.1:8554/test protocols=udp latency=500 \
  ! rtph264depay ! h264parse ! avdec_h264 ! autovideosink
```

## Webcam mode

`--list-devices` prints the video capture devices (`GstDeviceMonitor`,
class `Video/Source`) with their `--device-index` value and native sizes, then
exits:

```
0  MacBook Pro Camera 1080x1920 1920x1080 1552x1552 1328x1760 1760x1328 1280x720 640x480
```

`--webcam [--device-index N]` serves that device, 1280x720 at `--fps` unless
`--width/--height` are given, with the same overlays as pattern mode. On macOS
the launch string is

```
avfvideosrc device-index=N ! video/x-raw,format=NV12,pixel-aspect-ratio=1/1[,width=Wn,height=Hn]
  ! videoconvert ! videoscale ! videorate skip-to-first=true
  ! video/x-raw,width=W,height=H,framerate=F/1,pixel-aspect-ratio=1/1
  ! timeoverlay ! textoverlay ! videoconvert
  ! vtenc_h264 realtime=true allow-frame-reordering=false max-keyframe-interval=<keyint> bitrate=<kbps>
  ! h264parse config-interval=1 ! rtph264pay pt=96 config-interval=1
```

- The source element is `avfvideosrc` on macOS, `v4l2src device=/dev/videoN`
  on Linux and `ksvideosrc device-index=N` on Windows; the tool refuses to
  start with a clear message if the element is not installed, or if devices
  were listed but none has the requested index. Only macOS has been verified.
- `vtenc_h264` (VideoToolbox) is used when present, otherwise the x264 settings
  from pattern mode. `--keyint` maps to `max-keyframe-interval` and
  `--bitrate-kbps` to `bitrate`.
- `format=NV12` is pinned on the device: left to negotiate, avfvideosrc picks
  ARGB and the first frame fails inside `coremediabuffer` with
  `Unknown OSType format: 32` (GStreamer 1.26.1). NV12 is also the native
  input of `vtenc_h264`.
- `Wn x Hn` is the device's native size closest to the requested one (smallest
  size covering it with the nearest aspect ratio, else the largest), read
  through the device monitor. Without the pin avfvideosrc fixates to the first
  format it lists (1080x1920 portrait on a MacBook) and `videoscale` squashes
  it into the target. `pixel-aspect-ratio=1/1` on both sides of `videoscale`
  makes it letterbox rather than stretch when the aspect ratio differs, e.g.
  a 640x480 device into 640x360.
- `videorate` drops or duplicates to hit `--fps` when the camera runs at a
  different rate; `skip-to-first=true` avoids a burst of duplicated frames
  before the first capture.
- macOS asks for camera permission the first time the parent terminal opens a
  device (`avfvideosrc` logs `Requesting device video access permission`).
  If it was denied, the media fails to prepare and the plugin reports
  `Device video access permission has been explicitly denied before` in the
  server log (`GST_DEBUG=avfvideosrc:4`); grant it in System Settings >
  Privacy & Security > Camera for the terminal application. The denied path
  has not been exercised here, the permission was already granted.

Verified 2026-09-13 on macOS arm64 (M4), GStreamer 1.26.1: the TCP client
above decoded 170 frames in 8 s from the built-in camera (baseline profile,
level 3.1, 1280x720@25) and 78 frames in 5 s with `--width 640 --height 360`.

## Looping

Without `--loop` the file plays once; the server sends RTCP BYE at the end and
`rtspsrc` reports EOS. With `--loop` the clip repeats seamlessly:

- Once the media reaches PLAYING, a non-flushing `GST_SEEK_FLAG_SEGMENT` seek
  is sent to `qtdemux`, so at the end of the clip it posts `SEGMENT_DONE`
  instead of EOS. The handler on the media pipeline bus schedules another
  non-flushing segment seek to 0 on the main loop. Because the seeks are not
  flushing, the running time and the RTP timestamps keep increasing across the
  loop point; a client sees one continuous stream (verified: PTS monotonic,
  no gaps over 100 ms at the boundary).
- SEGMENT events leaving the payloader are rewritten with no `stop` or
  `duration`, and upstream SEGMENT queries passing the payloader get their
  stop cleared, so both the SDP and the PLAY response advertise
  `Range: npt=0-` (open-ended). This is required: with a finite range
  `rtspsrc` synthesises EOS when the range elapses even though data keeps
  arriving.
- `multifilesrc loop=true` is not used; it restarts the container byte
  stream, which does not give continuous timestamps through `qtdemux`.

The media factory is shared: all clients of one mount attach to the same
pipeline and see the same position. When the last client leaves the pipeline
is torn down and rebuilt for the next client (so a file starts at 0 again).

## Notes

- Bound to `127.0.0.1` only; UDP and TCP lower transports are enabled,
  multicast is not.
- `GST_DEBUG=rtsp-testsrc:5` logs the loop seeks and segment rewrites.
- Plugin warnings on start such as `libgstadaptivedemux2` failing to load are
  harmless.
