#!/usr/bin/env bash
# Spike B: build one renderer configuration, run it on the attached Android
# device/emulator, screenshot at launch and after a background/foreground
# cycle, and sample both screenshots.
#
# usage: AD=<agent-device command> tool/run_android.sh <label> <out-dir> \
#          [gradle -P key=value ...] [-- dart-define ...]
#   e.g. tool/run_android.sh api37_skia out spikeImpeller=false
#        tool/run_android.sh api37_gles_st out spikeBackend=opengles -- FORCE_GL=true
#
# UI actions (launch, home, screenshot) go through agent-device ($AD) so the
# run is visible in the T3 Device panel; adb is only used for install + logs.
set -euo pipefail
cd "$(dirname "$0")/.."

label=$1 out=$2
shift 2
pargs=() dargs=()
while [[ $# -gt 0 && $1 != -- ]]; do pargs+=("--android-project-arg=$1"); shift; done
[[ ${1:-} == -- ]] && shift
for d in "$@"; do dargs+=("--dart-define=$d"); done

ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
PKG=dev.webmtexture.spike_b
mkdir -p "$out"

fvm flutter build apk --debug ${pargs[@]+"${pargs[@]}"} ${dargs[@]+"${dargs[@]}"} >"$out/$label.build.log" 2>&1 \
  || { tail -30 "$out/$label.build.log"; exit 1; }
"$ADB" install -r build/app/outputs/flutter-apk/app-debug.apk >/dev/null
"$ADB" logcat -c

# agent-device sometimes reports "process remained running after close"
# right after an install; retry the relaunch once.
$AD open "$PKG" --relaunch >/dev/null 2>&1 || { sleep 2; $AD open "$PKG" --relaunch >/dev/null; }
sleep 6
$AD screenshot "$out/$label.launch.png" >/dev/null
"$ADB" logcat -d >"$out/$label.log"
python3 -I tool/sample.py "$out/$label.launch.png" "$out/$label.log" "$label launch" | tee "$out/$label.sample.txt"

# Background for 5 s, then foreground (no relaunch).
$AD home >/dev/null
sleep 5
$AD open "$PKG" >/dev/null
sleep 4
$AD screenshot "$out/$label.resume.png" >/dev/null
"$ADB" logcat -d >"$out/$label.log"
python3 -I tool/sample.py "$out/$label.resume.png" "$out/$label.log" "$label resume" | tee -a "$out/$label.sample.txt"

echo "--- renderer / surface events"
grep -E "Using the Impeller|Skia|SpikeB|SPIKE_B_LIFECYCLE|E MethodChannel|FATAL|ImageReader.*(error|fail)" "$out/$label.log" \
  | grep -v "SPIKE_B_LAYOUT" | cut -c19-200 | tee "$out/$label.events.txt"
