#!/bin/sh
# Downloads the pinned model set into the Hugging Face cache (resumable).
# Usage: scripts/fetch-models.sh [minimal|m0|all]
set -eu
cd "$(dirname "$0")/.."
SET="${1:-m0}"
PY="${PYTHON:-worker/.venv/bin/python}"
[ -x "$PY" ] || PY=/opt/anaconda3/bin/python3
case "$SET" in
  minimal) MODELS="NemoStation/Marlin-2B-MLX-8bit" ;;
  m0) MODELS="NemoStation/Marlin-2B-MLX-8bit google/siglip2-base-patch16-224 Qwen/Qwen3.5-4B" ;;
  all) MODELS="NemoStation/Marlin-2B-MLX-8bit google/siglip2-base-patch16-224 Qwen/Qwen3.5-4B Qwen/Qwen3-VL-Embedding-2B" ;;
  *) echo "unknown set $SET"; exit 2 ;;
esac
"$PY" - $MODELS <<'PYEOF'
import sys
from huggingface_hub import snapshot_download
for repo in sys.argv[1:]:
    path = snapshot_download(repo, allow_patterns=["*.json", "*.safetensors", "*.txt", "*.py", "*.model", "*.tiktoken", "merges.txt", "vocab.json", "*.jinja"])
    print(f"{repo} -> {path}")
PYEOF
