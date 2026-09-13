#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
. ./scripts/env.sh
PRESET="${1:-macos-dev}"
cmake --preset "$PRESET"
cmake --build --preset "$PRESET"
