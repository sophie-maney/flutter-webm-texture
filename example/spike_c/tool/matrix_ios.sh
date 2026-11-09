#!/usr/bin/env bash
# Spike C: the iOS Simulator matrix. usage: AD=... UDID=... tool/matrix_ios.sh <prefix> <out>
set -uo pipefail
cd "$(dirname "$0")/.."
p=$1 out=$2
run() { echo "=== $1"; tool/run_ios.sh "$@" >/dev/null 2>&1 || echo "FAILED $1"; }
run ${p}_base "$out" MODE=textures COUNT=0
run ${p}_tex "$out" MODE=textures
run ${p}_atlas "$out" MODE=atlas
run ${p}_atlas512 "$out" MODE=atlas ATLAS_H=512
run ${p}_atlas120 "$out" MODE=atlas COUNT=120
run ${p}_tex120 "$out" MODE=textures COUNT=120
TIMELINE_SECS=10 run ${p}_atlas_g2 "$out" MODE=atlas GUTTER=2 RUN_SECS=30
echo "=== done"
