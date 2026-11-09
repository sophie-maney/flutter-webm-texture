#!/usr/bin/env bash
# Spike C: run the Android matrix for one emulator. usage: tool/matrix_android.sh <prefix> <out> [skia-only]
set -uo pipefail
cd "$(dirname "$0")/.."
p=$1 out=$2 set=${3:-full}
run() { echo "=== $1"; tool/run_android.sh "$@" >/dev/null || echo "FAILED $1"; }
case $set in
full)
  run ${p}_base "$out" -- MODE=textures COUNT=0
  run ${p}_auto_tex "$out" -- MODE=textures
  run ${p}_auto_atlas "$out" -- MODE=atlas
  run ${p}_auto_atlas512 "$out" -- MODE=atlas ATLAS_H=512
  run ${p}_vk_atlas "$out" spikeBackend=vulkan -- MODE=atlas
  run ${p}_vk_tex "$out" spikeBackend=vulkan -- MODE=textures
  run ${p}_skia_tex "$out" spikeImpeller=false -- MODE=textures
  run ${p}_skia_atlas "$out" spikeImpeller=false -- MODE=atlas
  run ${p}_auto_atlas120 "$out" -- MODE=atlas COUNT=120
  run ${p}_auto_tex120 "$out" -- MODE=textures COUNT=120
  run ${p}_auto_atlas_g2 "$out" -- MODE=atlas GUTTER=2 RUN_SECS=30
  ;;
lowend)
  run ${p}_base "$out" -- MODE=textures COUNT=0
  run ${p}_auto_tex "$out" -- MODE=textures
  run ${p}_auto_atlas "$out" -- MODE=atlas
  run ${p}_auto_atlas512 "$out" -- MODE=atlas ATLAS_H=512
  run ${p}_auto_tex120 "$out" -- MODE=textures COUNT=120
  run ${p}_auto_atlas120 "$out" -- MODE=atlas COUNT=120
  ;;
old)
  run ${p}_base "$out" -- MODE=textures COUNT=0
  run ${p}_skia_tex "$out" -- MODE=textures
  run ${p}_skia_atlas "$out" -- MODE=atlas
  run ${p}_skia_atlas512 "$out" -- MODE=atlas ATLAS_H=512
  ;;
esac
echo "=== done"
