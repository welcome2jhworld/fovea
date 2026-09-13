#!/bin/sh
# record_rtsp.sh MUXER OUTDIR SECONDS SIGNAL [DECODER] [LATENCY_MS]
#   MUXER:   mp4 | fmp4 | mkv        (muxer="..." element syntax, async-finalize=false)
#            fmp4-async | mkv-async  (async-finalize=true + muxer-factory + muxer-properties)
#   SIGNAL:  INT (clean EOS via gst-launch -e) | KILL (simulated crash)
#   DECODER: avdec_h264 (default) | vtdec | vtdec_hw
# RTSP H.264 -> tee -> (A) splitmuxsink 10 s segments, no re-encode
#                   -> (B) decode -> BGRA -> sink (leaky queue bounded to 5 buffers)
set -e
. "$(dirname "$0")/env.sh"
MUXER=$1; OUT=$2; SECS=${3:-25}; SIG=${4:-INT}; DEC=${5:-avdec_h264}; LAT=${6:-1000}
URL=${URL:-rtsp://127.0.0.1:8554/test}
mkdir -p "$OUT"

case "$MUXER" in
  mp4)        MUX='muxer=mp4mux'; EXT=mp4 ;;
  fmp4)       MUX='muxer="mp4mux fragment-duration=1000"'; EXT=mp4 ;;
  mkv)        MUX='muxer=matroskamux'; EXT=mkv ;;
  fmp4-async) MUX='async-finalize=true muxer-factory=mp4mux muxer-properties="properties,fragment-duration=1000"'; EXT=mp4 ;;
  mkv-async)  MUX='async-finalize=true muxer-factory=matroskamux'; EXT=mkv ;;
  *) echo "unknown muxer $MUXER"; exit 2 ;;
esac
# UNBUFFERED=1: bypass filesink's 64 KB userspace buffer so a kill -9 loses at most one buffer
if [ "${UNBUFFERED:-0}" = 1 ]; then
  case "$MUXER" in
    *-async) MUX="$MUX sink-properties=properties,buffer-mode=unbuffered" ;;
    *)       MUX="$MUX sink=\"filesink buffer-mode=unbuffered\"" ;;
  esac
fi

PIPELINE="rtspsrc location=$URL protocols=tcp latency=$LAT \
 ! rtph264depay ! h264parse ! tee name=t \
 t. ! queue ! splitmuxsink location=$OUT/seg_%05d.$EXT max-size-time=10000000000 $MUX \
 t. ! queue leaky=downstream max-size-buffers=5 max-size-time=0 max-size-bytes=0 \
   ! $DEC ! videoconvert ! video/x-raw,format=BGRA ! fakesink sync=true"
echo "gst-launch-1.0 -e $PIPELINE"
eval exec "$(dirname "$0")/runfor.sh" "$SECS" "$SIG" gst-launch-1.0 -e $PIPELINE
