#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
. ./scripts/env.sh
PRESET="${1:-macos-dev}"
ctest --preset "$PRESET" --output-on-failure
