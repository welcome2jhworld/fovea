#!/bin/sh
# M2 verification: four inputs, headless recording with the console closed,
# kill -9 recovery, disk floor, retention. Runs scripts/verify_m2.py, which
# writes the report and log under docs/verification/.
# Usage: scripts/verify-m2.sh [minutes]   Env: BUILD (default build/macos-dev), PYTHON.
set -eu
cd "$(dirname "$0")/.."
. ./scripts/env.sh
exec "${PYTHON:-python3}" scripts/verify_m2.py --bin-dir "${BUILD:-build/macos-dev}" --inputs 4 --minutes "${1:-10}"
