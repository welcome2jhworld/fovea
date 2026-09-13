#!/bin/sh
# crash_test.sh OUTROOT : record with mp4 / fmp4 / mkv, kill -9 at 16 s, ffprobe the in-progress files.
# Needs the RTSP server running: build/rtsp_server live
set -e
. "$(dirname "$0")/env.sh"
ROOT=${1:?outroot}; HERE=$(dirname "$0")
for m in mp4 fmp4 mkv; do
  rm -rf "$ROOT/crash_$m"
  UNBUFFERED=${UNBUFFERED:-1} "$HERE/record_rtsp.sh" "$m" "$ROOT/crash_$m" 16 KILL > "$ROOT/crash_$m.log" 2>&1 &
done
wait
for m in mp4 fmp4 mkv; do
  for f in "$ROOT/crash_$m"/seg_*; do
    printf '%s size=%s frames=%s\n' "$f" "$(stat -f%z "$f")" \
      "$(ffprobe -v error -select_streams v -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$f" 2>&1 | head -1)"
  done
done
