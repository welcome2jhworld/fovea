#!/bin/sh
# file_input.sh CLIP.mp4 OUTDIR [SECONDS] : local mp4 paced at real time -> tee -> segments + decode
# Pacing: identity sync=true (or clocksync) before the tee. gst-launch cannot loop the file;
# see pipe_probe --loop (segment seek on SEGMENT_DONE) for a gapless loop.
set -e
. "$(dirname "$0")/env.sh"
CLIP=${1:?clip}; OUT=${2:?outdir}; SECS=${3:-24}
mkdir -p "$OUT"
exec "$(dirname "$0")/runfor.sh" "$SECS" INT gst-launch-1.0 -e \
  filesrc location="$CLIP" ! qtdemux ! h264parse ! identity sync=true ! tee name=t \
  t. ! queue ! splitmuxsink location="$OUT/seg_%05d.mkv" max-size-time=10000000000 muxer=matroskamux \
  t. ! queue leaky=downstream max-size-buffers=5 max-size-time=0 max-size-bytes=0 \
     ! avdec_h264 ! videoconvert ! video/x-raw,format=BGRA ! fakesink sync=false
