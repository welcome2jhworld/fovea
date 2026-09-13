#!/bin/sh
# M2 verification: four inputs, headless recording with the UI closed, service
# restart with recovery, disk floor. Usage: scripts/verify-m2.sh [minutes]
set -eu
cd "$(dirname "$0")/.."
. ./scripts/env.sh
ROOT="$(pwd)"
BUILD="$ROOT/build/macos-dev"
CORE="$BUILD/src/core/fovea-core"
CONSOLE="$BUILD/src/console/fovea"
TESTSRC="$BUILD/tools/rtsp-testsrc/rtsp-testsrc"
PY="${PYTHON:-/opt/homebrew/bin/python3.12}"
MINUTES="${1:-3}"
WORK="${FOVEA_VERIFY_DIR:-$ROOT/build/verify-m2}"
rm -rf "$WORK"; mkdir -p "$WORK/data"
LOG="$WORK/verify.log"
CLIP="$WORK/clip.mp4"
N=4

log() { printf '%s %s\n' "$(date -u +%H:%M:%S)" "$*" | tee -a "$LOG"; }
cleanup() {
  [ -n "${CORE_PID:-}" ] && kill "$CORE_PID" 2>/dev/null || true
  for p in ${SRC_PIDS:-}; do kill "$p" 2>/dev/null || true; done
  pkill -f "$CONSOLE" 2>/dev/null || true
  sleep 1
}
fail() { log "FAIL: $*"; cleanup; exit 1; }
trap cleanup INT TERM
api() { M="$1"; P="$2"; B="${3:-}"; if [ -n "$B" ]; then curl -s -X "$M" -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d "$B" "http://127.0.0.1:$CPORT$P"; else curl -s -X "$M" -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$CPORT$P"; fi; }
jget() { "$PY" -c "import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$1"; }
wait_core() { for i in $(seq 1 60); do [ -f "$WORK/data/core.json" ] && break; sleep 0.25; done; [ -f "$WORK/data/core.json" ] || fail "core.json not written"; CPORT=$(jget "d['port']" < "$WORK/data/core.json"); TOKEN=$(cat "$WORK/data/core.token"); }
start_core() { FOVEA_MIN_FREE_MB="${FOVEA_MIN_FREE_MB:-50}" "$CORE" --data-dir "$WORK/data" --port 0 >"$WORK/core-$1.log" 2>&1 & CORE_PID=$!; wait_core; }
rss_mb() { ps -o rss= -p "$1" 2>/dev/null | awk '{printf "%.0f", $1/1024}'; }
cpu_pct() { ps -o %cpu= -p "$1" 2>/dev/null | tr -d ' '; }

[ -x "$CORE" ] && [ -x "$TESTSRC" ] || fail "build first (scripts/build.sh)"
./scripts/make-test-media.sh "$CLIP" 20 >/dev/null
SRC_PIDS=""
i=0
while [ $i -lt $N ]; do
  "$TESTSRC" --port $((8554 + i)) --path /test --file "$CLIP" --loop >"$WORK/src$i.log" 2>&1 &
  SRC_PIDS="$SRC_PIDS $!"; i=$((i + 1))
done
sleep 1.5
log "started $N sources"
start_core 1
log "core pid $CORE_PID port $CPORT"
IDS=""
i=0
while [ $i -lt $N ]; do
  C=$(api POST /v1/cameras "{\"code\":\"CAM-0$((i + 1))\",\"name\":\"Source $((i + 1))\",\"group_name\":\"Lab\",\"kind\":\"rtsp\",\"main_url\":\"rtsp://127.0.0.1:$((8554 + i))/test\",\"transport\":\"tcp\",\"jitter_ms\":1000,\"segment_seconds\":30}")
  ID=$(echo "$C" | jget "d['id']") || fail "create camera $i: $C"
  IDS="$IDS $ID"; i=$((i + 1))
done
log "cameras: $IDS"

all_online() {
  for id in $IDS; do
    S=$(api GET "/v1/cameras/$id/status")
    ST=$(echo "$S" | jget "d['state']"); FPS=$(echo "$S" | jget "d['fps_new']")
    OK=$(echo "$FPS" | "$PY" -c 'import sys; print(float(sys.stdin.read())>20)')
    [ "$ST" = "online" ] && [ "$OK" = "True" ] || return 1
  done
  return 0
}
for i in $(seq 1 120); do all_online && break; sleep 0.5; done
all_online || fail "not all cameras online with >20 fps"
log "all $N cameras online"

if [ -x "$CONSOLE" ]; then
  QT_QPA_PLATFORM=offscreen FOVEA_SCREENSHOT="$WORK/console.png" "$CONSOLE" --data-dir "$WORK/data" >"$WORK/console.log" 2>&1 &
  CON_PID=$!
  for i in $(seq 1 60); do kill -0 "$CON_PID" 2>/dev/null || break; sleep 0.5; done
  [ -f "$WORK/console.png" ] && log "console screenshot captured, console exited" || log "console screenshot missing (see console.log)"
  kill -0 "$CORE_PID" 2>/dev/null || fail "core died when the console exited"
  log "core still running after console exit"
fi

log "soak for $MINUTES min (samples every 30 s)"
END=$(( $(date +%s) + MINUTES * 60 ))
while [ "$(date +%s)" -lt "$END" ]; do
  sleep 30
  M=$(api GET /v1/metrics)
  TOT=0
  for id in $IDS; do
    S=$(api GET "/v1/cameras/$id/status")
    log "  $(echo "$S" | jget "d['camera_id'][:8]") state=$(echo "$S" | jget "d['state']") fps=$(echo "$S" | jget "d['fps_new']") p95=$(echo "$S" | jget "d['latency_ms']['p95']") drops=$(echo "$S" | jget "d['drops']") q=$(echo "$S" | jget "d['queue_depth']") rec=$(echo "$S" | jget "d['recording']")"
  done
  log "  core rss=$(rss_mb "$CORE_PID")MB cpu=$(cpu_pct "$CORE_PID")% metrics=$(echo "$M" | head -c 300)"
done

for id in $IDS; do
  SEGS=$(api GET "/v1/cameras/$id/segments")
  log "camera ${id%${id#????????}} segments finalized=$(echo "$SEGS" | jget "sum(1 for s in d if s['state']=='finalized')") recording=$(echo "$SEGS" | jget "sum(1 for s in d if s['state']=='recording')")"
done

log "kill -9 core mid-recording, restart, check recovery"
kill -9 "$CORE_PID"; sleep 1; CORE_PID=""
rm -f "$WORK/data/core.json"
start_core 2
sleep 2
grep -i 'recovery:' "$WORK/core-2.log" | tail -1 | tee -a "$LOG"
for id in $IDS; do
  SEGS=$(api GET "/v1/cameras/$id/segments")
  NREC=$(echo "$SEGS" | jget "sum(1 for s in d if s['state']=='recording')")
  NDMG=$(echo "$SEGS" | jget "sum(1 for s in d if s['state']=='damaged')")
  log "  after restart: recording=$NREC damaged=$NDMG finalized=$(echo "$SEGS" | jget "sum(1 for s in d if s['state']=='finalized')")"
done
for i in $(seq 1 120); do all_online && break; sleep 0.5; done
all_online || fail "cameras did not come back after restart"
log "all cameras back online after restart"

log "disk floor: restart core with an impossible floor"
api POST /v1/service/shutdown >/dev/null; for i in $(seq 1 40); do kill -0 "$CORE_PID" 2>/dev/null || break; sleep 0.25; done; CORE_PID=""
FOVEA_MIN_FREE_MB=100000000 start_core 3
sleep 4
for id in $IDS; do
  S=$(api GET "/v1/cameras/$id/status")
  log "  recording=$(echo "$S" | jget "d['recording']") state=$(echo "$S" | jget "d['state']")"
  [ "$(echo "$S" | jget "d['recording']")" = "paused_disk" ] || fail "recording not paused with the disk floor"
done
log "disk floor honoured (recording paused_disk, viewing continues)"
api POST /v1/service/shutdown >/dev/null; sleep 1
cleanup
mkdir -p docs/verification
cp "$LOG" "docs/verification/m2-$(date -u +%Y%m%d-%H%M%S).log"
log "PASS"
