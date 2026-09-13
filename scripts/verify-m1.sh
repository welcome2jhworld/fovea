#!/bin/sh
# M1 integration verification: local RTSP source -> core -> recording -> reconnect -> playback -> crash recovery.
# Usage: scripts/verify-m1.sh [--webcam]   (report written to docs/verification/)
# Env: BUILD (default build/macos-dev), FOVEA_VERIFY_DIR (default build/verify-m1), FOVEA_RTSP_PORT (default 8554).
set -eu
cd "$(dirname "$0")/.."
. ./scripts/env.sh
ROOT="$(pwd)"
abspath() { case "$1" in /*) echo "$1" ;; *) echo "$ROOT/$1" ;; esac; }
BUILD="$(abspath "${BUILD:-build/macos-dev}")"
CORE="$BUILD/src/core/fovea-core"
TESTSRC="$BUILD/tools/rtsp-testsrc/rtsp-testsrc"
PY="${PYTHON:-/opt/homebrew/bin/python3.12}"
WORK="$(abspath "${FOVEA_VERIFY_DIR:-build/verify-m1}")"
rm -rf "$WORK"; mkdir -p "$WORK/data"
CLIP="$WORK/clip.mp4"
PORT="${FOVEA_RTSP_PORT:-8554}"
URL="rtsp://127.0.0.1:$PORT/test"
REPORT="$WORK/report.json"
LOG="$WORK/verify.log"
SOURCE_MODE="${1:-file}"

log() { printf '%s %s\n' "$(date -u +%H:%M:%S)" "$*" | tee -a "$LOG"; }
fail() { log "FAIL: $*"; cleanup; exit 1; }
cleanup() {
  [ -n "${CORE_PID:-}" ] && kill "$CORE_PID" 2>/dev/null || true
  [ -n "${SRC_PID:-}" ] && kill "$SRC_PID" 2>/dev/null || true
  sleep 1
}
trap cleanup INT TERM

api() { # method path [json]
  M="$1"; P="$2"; B="${3:-}"
  if [ -n "$B" ]; then
    curl -s -X "$M" -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d "$B" "http://127.0.0.1:$CPORT$P"
  else
    curl -s -X "$M" -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$CPORT$P"
  fi
}
jget() { "$PY" -c "import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$1"; }

[ -x "$CORE" ] || fail "missing $CORE (run scripts/build.sh)"
[ -x "$TESTSRC" ] || fail "missing $TESTSRC (run scripts/build.sh)"

start_source() {
  if [ "$SOURCE_MODE" = "--webcam" ]; then
    "$TESTSRC" --port $PORT --path /test --webcam >"$WORK/testsrc.log" 2>&1 &
  else
    "$TESTSRC" --port $PORT --path /test --file "$CLIP" --loop >"$WORK/testsrc.log" 2>&1 &
  fi
  SRC_PID=$!
  sleep 1.5
}

log "generating clip"
./scripts/make-test-media.sh "$CLIP" 20 >/dev/null
log "starting rtsp test source ($SOURCE_MODE)"
start_source
log "starting core"
start_core() { # log-file
  rm -f "$WORK/data/core.json"
  FOVEA_MIN_FREE_MB=50 "$CORE" --data-dir "$WORK/data" --port 0 >"$WORK/$1" 2>&1 &
  CORE_PID=$!
  for i in $(seq 1 40); do [ -f "$WORK/data/core.json" ] && break; sleep 0.25; done
  [ -f "$WORK/data/core.json" ] || fail "core.json not written"
  CPORT=$(jget "d['port']" < "$WORK/data/core.json")
  TOKEN=$(cat "$WORK/data/core.token")
}
start_core core.log
log "core on port $CPORT"

HEALTH=$(api GET /v1/health)
echo "$HEALTH" | grep -q '"ok"' || fail "health: $HEALTH"

CAM=$(api POST /v1/cameras "{\"code\":\"CAM-01\",\"name\":\"Test source\",\"group_name\":\"Lab\",\"kind\":\"rtsp\",\"main_url\":\"$URL\",\"transport\":\"tcp\",\"jitter_ms\":1000,\"segment_seconds\":10,\"record_enabled\":true}")
CAMID=$(echo "$CAM" | jget "d['id']") || fail "create camera: $CAM"
log "camera $CAMID"

wait_state() { # expected-state timeout-s
  for i in $(seq 1 $(( $2 * 4 ))); do
    S=$(api GET "/v1/cameras/$CAMID/status")
    ST=$(echo "$S" | jget "d['state']")
    FPS=$(echo "$S" | jget "d['fps_new']")
    if [ "$ST" = "$1" ]; then
      if [ "$1" != "online" ]; then echo "$S"; return 0; fi
      OK=$(echo "$FPS" | "$PY" -c 'import sys; print(float(sys.stdin.read())>1)')
      [ "$OK" = "True" ] && { echo "$S"; return 0; }
    fi
    sleep 0.25
  done
  echo "$S"; return 1
}

# Every finalized segment must sit on its session's pts -> UTC mapping.
check_segment_times() {
  SESS=$(api GET "/v1/cameras/$CAMID/sessions")
  SEGS=$(api GET "/v1/cameras/$CAMID/segments")
  BAD=$("$PY" -c '
import json, sys
sessions = {s["id"]: s for s in json.loads(sys.argv[1])}
bad = []
for g in json.loads(sys.argv[2]):
    if g["state"] != "finalized":
        continue
    s = sessions.get(g["session_id"])
    if s is None or s["first_pts_ns"] < 0:
        bad.append(g["id"] + ": session has no anchor")
        continue
    expected = s["started_utc_ms"] + (g["start_pts_ns"] - s["first_pts_ns"]) / 1e6
    if abs(g["start_utc_ms"] - expected) > 5:
        bad.append("%s: start_utc_ms %d, session mapping %.1f" % (g["id"], g["start_utc_ms"], expected))
print("; ".join(bad))
' "$SESS" "$SEGS")
  [ -z "$BAD" ] || fail "segment UTC off the session mapping: $BAD"
  log "segment start times match their session mapping ($(echo "$SEGS" | jget "sum(1 for s in d if s['state']=='finalized')") finalized)"
}

log "waiting for frames"
S=$(wait_state online 30) || fail "camera never came online with frames: $S"
SESSION1=$(echo "$S" | jget "d['session_id']")
log "online session=$SESSION1 fps=$(echo "$S" | jget "d['fps_new']") latency=$(echo "$S" | jget "d['latency_ms']") ring=$(echo "$S" | jget "d['frame_ring']['name']")"
RING=$(echo "$S" | jget "d['frame_ring']['name']")

log "sampling shared-memory ring for 3 s"
RINGSTAT=$("$PY" scripts/ring_probe.py "$RING" 3) || fail "ring probe failed"
log "ring: $RINGSTAT"

log "waiting for 2 finalized segments (segment_seconds=10)"
for i in $(seq 1 240); do
  SEGS=$(api GET "/v1/cameras/$CAMID/segments")
  NFIN=$(echo "$SEGS" | jget "sum(1 for s in d if s['state']=='finalized')")
  [ "$NFIN" -ge 2 ] && break
  sleep 0.5
done
[ "$NFIN" -ge 2 ] || fail "segments finalized: $NFIN ($SEGS)"
SEGPATH=$(echo "$SEGS" | jget "[s for s in d if s['state']=='finalized'][-1]['path']")
SEGID=$(echo "$SEGS" | jget "[s for s in d if s['state']=='finalized'][-1]['id']")
PROBE=$(ffprobe -v error -count_frames -show_entries stream=codec_name,nb_read_frames:format=duration -of csv=p=0 "$SEGPATH" | tr '\n' ' ')
log "segment $SEGPATH probe: $PROBE"
echo "$PROBE" | grep -q h264 || fail "segment not h264: $PROBE"

log "stopping source to test disconnect"
kill "$SRC_PID"; wait "$SRC_PID" 2>/dev/null || true; SRC_PID=""
DISC_T0=$(date +%s)
for i in $(seq 1 60); do
  S=$(api GET "/v1/cameras/$CAMID/status"); ST=$(echo "$S" | jget "d['state']")
  [ "$ST" != "online" ] && break; sleep 0.5
done
[ "$ST" != "online" ] || fail "camera stayed online after source stop"
DISC_T1=$(date +%s)
AGE=$(echo "$S" | jget "d['last_frame_age_ms']")
log "state after source stop: $ST (detected in $((DISC_T1-DISC_T0)) s), stale=$(echo "$S" | jget "d['stale']") age_ms=$AGE"
[ "$AGE" -ge 0 ] || fail "last_frame_age_ms is $AGE after frames were received"
sleep 3
GAPS=$(api GET "/v1/cameras/$CAMID/gaps")
NGAP=$(echo "$GAPS" | jget "len(d)")
log "gaps recorded: $NGAP"

log "restarting source to test reconnect"
start_source
REC_T0=$(date +%s)
S=$(wait_state online 60) || fail "no reconnect: $S"
REC_T1=$(date +%s)
SESSION2=$(echo "$S" | jget "d['session_id']")
[ "$SESSION2" != "$SESSION1" ] || fail "session id did not change on reconnect"
log "reconnected in $((REC_T1-REC_T0)) s, new session=$SESSION2 reconnects=$(echo "$S" | jget "d['reconnects']")"
sleep 2
GAPS=$(api GET "/v1/cameras/$CAMID/gaps")
NCLOSED=$(echo "$GAPS" | jget "sum(1 for g in d if g['to_utc_ms']>0)")
NGAP=$(echo "$GAPS" | jget "len(d)")
log "gaps: $NGAP total, $NCLOSED closed"
[ "$NCLOSED" -ge 1 ] || fail "no receive gap was closed after reconnect: $GAPS"
[ "$NCLOSED" -eq "$NGAP" ] || fail "receive gaps left open after reconnect: $GAPS"
check_segment_times

log "restarting the pipeline via PUT: the frame ring must be renamed for the new session"
OLDRING=$(echo "$S" | jget "d['frame_ring']['name']")
api PUT "/v1/cameras/$CAMID" '{"jitter_ms":1200}' >/dev/null
S=$(wait_state online 30) || fail "camera not back online after PUT: $S"
SESSION3=$(echo "$S" | jget "d['session_id']")
NEWRING=$(echo "$S" | jget "d['frame_ring']['name']")
[ "$SESSION3" != "$SESSION2" ] || fail "PUT restart kept session $SESSION2"
[ "$NEWRING" != "$OLDRING" ] || fail "PUT restart kept ring name $OLDRING"
"$PY" scripts/ring_probe.py "$OLDRING" 0.2 >/dev/null 2>&1 && fail "previous session's ring $OLDRING still exists"
RINGSTAT=$("$PY" scripts/ring_probe.py "$NEWRING" 2) || fail "no frames in the new session's ring $NEWRING"
log "ring renamed $OLDRING -> $NEWRING, old ring gone, new ring: $RINGSTAT"

log "playback of segment $SEGID"
PB=$(api POST /v1/playback "{\"segment_id\":\"$SEGID\"}")
PBID=$(echo "$PB" | jget "d['id']") || fail "playback open: $PB"
api POST "/v1/playback/$PBID/play" >/dev/null
sleep 2
PBS=$(api GET "/v1/playback/$PBID")
POS=$(echo "$PBS" | jget "d['position_ns']")
PBRING=$(echo "$PBS" | jget "d['frame_ring']['name']")
PBSTAT=$("$PY" scripts/ring_probe.py "$PBRING" 2) || fail "playback ring probe failed"
log "playback state=$(echo "$PBS" | jget "d['state']") position_ns=$POS ring: $PBSTAT"
[ "$POS" -gt 0 ] || fail "playback position did not advance"
api POST "/v1/playback/$PBID/seek" '{"pts_ns":1000000000}' >/dev/null
api DELETE "/v1/playback/$PBID" >/dev/null

METRICS=$(api GET /v1/metrics)
log "metrics: $(echo "$METRICS" | head -c 600)"

log "shutting down core via API"
api POST /v1/service/shutdown >/dev/null
for i in $(seq 1 40); do kill -0 "$CORE_PID" 2>/dev/null || break; sleep 0.25; done
kill -0 "$CORE_PID" 2>/dev/null && fail "core did not exit after shutdown"
CORE_PID=""
[ ! -f "$WORK/data/core.json" ] || fail "core.json left behind"

log "restart core to check recovery and segment states"
start_core core2.log
sleep 1
SEGS=$(api GET "/v1/cameras/$CAMID/segments")
CURSESSION=$(api GET "/v1/cameras/$CAMID/status" | jget "d['session_id']")
NREC=$(echo "$SEGS" | jget "sum(1 for s in d if s['state']=='recording' and s['session_id']!='$CURSESSION')")
NLIVE=$(echo "$SEGS" | jget "sum(1 for s in d if s['state']=='recording' and s['session_id']=='$CURSESSION')")
NDMG=$(echo "$SEGS" | jget "sum(1 for s in d if s['state']=='damaged')")
NFIN=$(echo "$SEGS" | jget "sum(1 for s in d if s['state']=='finalized')")
log "after restart: finalized=$NFIN damaged=$NDMG stale-recording=$NREC open-in-new-session=$NLIVE"
[ "$NREC" -eq 0 ] || fail "segments from earlier sessions still marked recording after restart"
grep -i 'recovery:' "$WORK/core2.log" | tail -1 | tee -a "$LOG"
check_segment_times

log "waiting for a segment that has been recording for 4 s, then killing the core with SIGKILL"
S=$(wait_state online 30) || fail "camera not online after restart: $S"
CRASHSEG=""
for i in $(seq 1 120); do
  SEGS=$(api GET "/v1/cameras/$CAMID/segments")
  CRASHSEG=$(echo "$SEGS" | "$PY" -c '
import json, sys, time
now = int(time.time() * 1000)
open_segs = [s for s in json.load(sys.stdin) if s["state"] == "recording" and now - s["created_utc_ms"] >= 4000]
print(open_segs[0]["id"] if open_segs else "")
')
  [ -n "$CRASHSEG" ] && break
  sleep 0.25
done
[ -n "$CRASHSEG" ] || fail "no segment stayed in recording for 4 s: $SEGS"
kill -9 "$CORE_PID"; wait "$CORE_PID" 2>/dev/null || true; CORE_PID=""
start_core core3.log
sleep 1
RECOVERY=$(grep -i 'recovery:' "$WORK/core3.log" | tail -1)
log "after SIGKILL: $RECOVERY"
NFINREC=$(echo "$RECOVERY" | sed -n 's/.*segments finalized \([0-9]*\).*/\1/p')
[ "${NFINREC:-0}" -ge 1 ] || fail "recovery finalized no segment after SIGKILL: $RECOVERY"
SEG=$(api GET "/v1/segments/$CRASHSEG")
CHECK=$(echo "$SEG" | jget "'ok' if d['state']=='finalized' and d['end_utc_ms']>d['start_utc_ms'] and d['bytes']>0 else d")
[ "$CHECK" = "ok" ] || fail "segment cut by SIGKILL not recovered: $SEG"
log "recovered segment $CRASHSEG: $(echo "$SEG" | jget "'%.1f s, %d bytes' % ((d['end_pts_ns']-d['start_pts_ns'])/1e9, d['bytes'])")"
GAPS=$(api GET "/v1/cameras/$CAMID/gaps")
NOPEN=$(echo "$GAPS" | jget "sum(1 for g in d if g['to_utc_ms']==0)")
[ "$NOPEN" -eq 0 ] || fail "receive gaps still open after recovery: $GAPS"
CURSESSION=$(api GET "/v1/cameras/$CAMID/status" | jget "d['session_id']")
NOPENSESS=$(api GET "/v1/cameras/$CAMID/sessions" | jget "sum(1 for s in d if s['ended_utc_ms']==0 and s['id']!='$CURSESSION')")
[ "$NOPENSESS" -eq 0 ] || fail "sessions from before the crash left open"
check_segment_times
api POST /v1/service/shutdown >/dev/null
for i in $(seq 1 40); do kill -0 "$CORE_PID" 2>/dev/null || break; sleep 0.25; done
kill -0 "$CORE_PID" 2>/dev/null && fail "core did not exit after shutdown"
CORE_PID=""

cleanup
log "PASS"
"$PY" - "$REPORT" "$LOG" <<'PYEOF'
import json, sys, re, datetime
report, log = sys.argv[1], sys.argv[2]
lines = open(log).read().splitlines()
json.dump({"generated_utc": datetime.datetime.now(datetime.UTC).isoformat(), "lines": lines}, open(report, "w"), indent=1)
PYEOF
mkdir -p docs/verification
cp "$LOG" "docs/verification/m1-$(date -u +%Y%m%d-%H%M%S).log"
