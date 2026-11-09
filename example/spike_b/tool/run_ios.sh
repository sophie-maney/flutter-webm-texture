#!/usr/bin/env bash
# Spike B on the iOS Simulator: build, install, screenshot at launch and after
# a background/foreground cycle, sample both.
#
# usage: AD=<agent-device command> UDID=<sim udid> tool/run_ios.sh <label> <out-dir> [dart-define ...]
#   e.g. tool/run_ios.sh ios_bg out
#        tool/run_ios.sh ios_main out IOS_NOTIFY=main
#
# UI actions (launch, home, screenshot) go through agent-device ($AD) so the
# run is visible in the T3 Device panel; simctl is only used for install + logs.
set -euo pipefail
cd "$(dirname "$0")/.."

label=$1 out=$2
shift 2
dargs=()
for d in "$@"; do dargs+=("--dart-define=$d"); done
BUNDLE=dev.webmtexture.spikeb
DENSITY=${DENSITY:-3}
mkdir -p "$out"

fvm flutter build ios --simulator --debug ${dargs[@]+"${dargs[@]}"} >"$out/$label.build.log" 2>&1 \
  || { tail -30 "$out/$label.build.log"; exit 1; }
xcrun simctl install "$UDID" build/ios/iphonesimulator/Runner.app

xcrun simctl spawn "$UDID" log stream --level debug --style compact \
  --predicate 'process == "Runner"' >"$out/$label.log" 2>&1 &
logpid=$!
trap 'kill $logpid 2>/dev/null || true' EXIT
sleep 1

$AD open "$BUNDLE" --relaunch >/dev/null
sleep 8
$AD screenshot "$out/$label.launch.png" --pixel-density "$DENSITY" >/dev/null
python3 -I tool/sample.py "$out/$label.launch.png" "$out/$label.log" "$label launch" | tee "$out/$label.sample.txt"

$AD home >/dev/null
sleep 5
$AD open "$BUNDLE" >/dev/null
sleep 4
$AD screenshot "$out/$label.resume.png" --pixel-density "$DENSITY" >/dev/null
python3 -I tool/sample.py "$out/$label.resume.png" "$out/$label.log" "$label resume" | tee -a "$out/$label.sample.txt"

echo "--- events"
grep -E "SpikeB|SPIKE_B_LIFECYCLE|rendering backend|Impeller|Error|error" "$out/$label.log" \
  | grep -v SPIKE_B_LAYOUT | cut -c1-220 | tee "$out/$label.events.txt"
