#!/usr/bin/env bash
# Spike C on the iOS Simulator: build, install, run one configuration, wait
# for the measurement window to finish, and collect the result.
#
# usage: AD=<agent-device command> UDID=<sim udid> tool/run_ios.sh <label> <out-dir> [dart-define ...]
#   e.g. tool/run_ios.sh ios_tex out MODE=textures
#
# The simulator only runs debug builds (no AOT simulator artefacts), so the
# Dart side is JIT. Runner's Debug configuration is built with -O so the
# Swift copy loops are not -Onone. UI actions go through agent-device ($AD);
# simctl only installs and streams logs.
set -euo pipefail
cd "$(dirname "$0")/.."

label=$1 out=$2
shift 2
dargs=("--dart-define=LABEL=$label")
for d in "$@"; do dargs+=("--dart-define=$d"); done
BUNDLE=dev.webmtexture.spikec
DENSITY=${DENSITY:-3}
mkdir -p "$out"
rm -f "$out/$label".*

fvm flutter build ios --simulator --debug "${dargs[@]}" >"$out/$label.build.log" 2>&1 \
  || { tail -30 "$out/$label.build.log"; exit 1; }
xcrun simctl terminate "$UDID" "$BUNDLE" >/dev/null 2>&1 || true
xcrun simctl install "$UDID" build/ios/iphonesimulator/Runner.app

xcrun simctl spawn "$UDID" log stream --level debug --style compact \
  --predicate 'process == "Runner"' >"$out/$label.log" 2>&1 &
logpid=$!
toppid=
trap 'kill $logpid $toppid 2>/dev/null || true' EXIT
sleep 2

$AD open "$BUNDLE" --relaunch >/dev/null
# The simulator app is a host process: sample its %CPU and energy impact.
for _ in $(seq 1 20); do
  apid=$(pgrep -f "$UDID/data/Containers/Bundle/Application/.*/Runner.app/Runner" | head -1 || true)
  [[ -n $apid ]] && break
  sleep 0.5
done
top -l 0 -s 5 -pid "$apid" -stats pid,cpu,power >"$out/$label.hosttop.txt" 2>&1 &
toppid=$!
sleep 25
$AD screenshot "$out/$label.png" --pixel-density "$DENSITY" >/dev/null
footprint "$apid" >"$out/$label.footprint.mid.txt" 2>&1 || true
# Raster timings come from the engine timeline (no FrameTimings on iOS for
# texture-only frames): 90 s inside the 120-s measurement window.
vm=$(grep -o "Dart VM service is listening on http[^ ]*" "$out/$label.log" | tail -1 | awk '{print $NF}')
fvm dart tool/timeline.dart "$vm" "${TIMELINE_SECS:-90}" "$label" >"$out/$label.timeline.json" || true
for _ in $(seq 1 120); do
  grep -q -E "SPIKE_C_RESULT ([0-9]+)/\1 |SPIKE_C_ERROR" "$out/$label.log" && break
  sleep 2
done
sleep 1
footprint "$apid" >"$out/$label.footprint.txt" 2>&1 || true
python3 -I tool/collect.py "$label" "$out" | tee -a "$out/results.jsonl"
