#!/bin/sh
# Creates the worker virtualenv with uv. Model weights are NOT downloaded here.
# Extras: torch (reference backend, all platforms), mlx (macOS experiment), or "torch,mlx".
set -eu
cd "$(dirname "$0")/../worker"
EXTRA="${1:-torch}"
export PATH="$HOME/.local/bin:/opt/homebrew/bin:$PATH"
uv venv .venv --python 3.12
uv pip install --python .venv/bin/python -e ".[$EXTRA]"
echo "worker ready: $(pwd)/.venv (extras: $EXTRA)"
