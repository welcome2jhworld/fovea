#!/bin/sh
# M0 real-model smoke test: runs one clip through the worker backends that are
# installed and records the raw output. Usage: scripts/vlm-smoke.sh <frames-dir> [backend ...]
set -eu
cd "$(dirname "$0")/.."
FRAMES="${1:?frames dir (jpg/png at 2 fps)}"
shift || true
BACKENDS="${*:-mlx transformers}"
PY=worker/.venv/bin/python
[ -x "$PY" ] || { echo "run scripts/setup-worker.sh torch,mlx first"; exit 2; }
OUT=docs/verification/vlm-smoke-$(date -u +%Y%m%d-%H%M%S).md
mkdir -p docs/verification
{
  echo "# VLM smoke test $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo
  echo "Frames: $FRAMES ($(ls "$FRAMES" | wc -l | tr -d ' ') files). Machine: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m), $(sysctl -n hw.memsize 2>/dev/null | awk '{print $1/1073741824 " GB"}')."
  for b in $BACKENDS; do
    echo
    echo "## backend: $b"
    echo
    echo '```'
    START=$(date +%s)
    PYTHONPATH=worker "$PY" -m fovea_worker.cli analyze-clip "$FRAMES" --backend "$b" --fps 2 --max-frames 16 --max-new-tokens 512 ${CLIP:+--clip "$CLIP"} \
      --prompt "You are reviewing CCTV footage. Describe what happens across these frames in order: people, vehicles, objects, and any notable actions. Answer in Korean." 2>&1 | tail -80 || echo "backend $b failed with exit $?"
    echo "wall_seconds=$(( $(date +%s) - START ))"
    echo '```'
  done
} | tee "$OUT"
echo "written $OUT"
