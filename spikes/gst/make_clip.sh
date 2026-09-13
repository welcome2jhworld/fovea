#!/bin/sh
# make_clip.sh OUT.mp4 : 640x360 25 fps 20 s H.264, keyframe every 25 frames, burnt-in frame/time counter
OUT=${1:?out path}
FONT=/System/Library/Fonts/Supplemental/Arial.ttf
if [ -f "$FONT" ]; then
  VF="drawtext=fontfile=$FONT:text='F %{n} T %{pts\\:hms}':x=10:y=10:fontsize=28:fontcolor=white:box=1:boxcolor=black@0.6"
else
  VF=null
fi
exec /opt/homebrew/bin/ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=640x360:rate=25" -t 20 \
  -vf "$VF" -c:v libx264 -preset veryfast -pix_fmt yuv420p -g 25 -keyint_min 25 -sc_threshold 0 -bf 0 \
  -movflags +faststart "$OUT"
