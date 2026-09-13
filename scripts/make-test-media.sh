#!/bin/sh
# Generates a small H.264 test clip with a burnt-in frame counter and clock.
# Usage: scripts/make-test-media.sh <out.mp4> [seconds] [width] [height] [fps]
set -eu
OUT="${1:?output path}"
SECS="${2:-20}"
W="${3:-640}"
H="${4:-360}"
FPS="${5:-25}"
FONT=""
for f in /System/Library/Fonts/Supplemental/Arial.ttf /System/Library/Fonts/Helvetica.ttc /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf; do
  if [ -f "$f" ]; then FONT="$f"; break; fi
done
if [ -n "$FONT" ]; then
  VF="drawtext=fontfile=$FONT:text='%{n} %{pts\\:hms}':x=10:y=10:fontsize=28:fontcolor=white:box=1:boxcolor=black@0.5"
  ffmpeg -v error -y -f lavfi -i "testsrc2=size=${W}x${H}:rate=${FPS}" -t "$SECS" -vf "$VF" \
    -c:v libx264 -preset veryfast -tune zerolatency -g "$FPS" -keyint_min "$FPS" -sc_threshold 0 -bf 0 -pix_fmt yuv420p -b:v 800k "$OUT"
else
  ffmpeg -v error -y -f lavfi -i "testsrc2=size=${W}x${H}:rate=${FPS}" -t "$SECS" \
    -c:v libx264 -preset veryfast -tune zerolatency -g "$FPS" -keyint_min "$FPS" -sc_threshold 0 -bf 0 -pix_fmt yuv420p -b:v 800k "$OUT"
fi
echo "$OUT"
