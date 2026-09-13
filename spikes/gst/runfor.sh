#!/bin/sh
# runfor.sh SECONDS SIGNAL cmd... : run cmd, send SIGNAL after SECONDS, wait for exit.
# SIGNAL=INT lets gst-launch-1.0 finish with EOS; SIGNAL=KILL simulates a crash.
secs=$1; sig=$2; shift 2
"$@" & pid=$!
sleep "$secs"
kill -"$sig" "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
