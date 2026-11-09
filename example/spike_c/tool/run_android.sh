#!/usr/bin/env bash
# Spike C: build one configuration as a profile APK, run it on the attached
# emulator/device, wait for the measurement window to finish, and collect
# the result.
#
# usage: AD=<agent-device command> tool/run_android.sh <label> <out-dir> \
#          [gradle -P key=value ...] [-- dart-define ...]
#   e.g. tool/run_android.sh api37_auto_tex out -- MODE=textures
#        tool/run_android.sh api37_vk_atlas out spikeBackend=vulkan -- MODE=atlas
#
# `fvm flutter run --profile` refuses emulators, so this builds the same
# profile APK and installs it with adb. UI actions (launch, screenshot) go
# through agent-device ($AD); adb only installs, reads logs and dumpsys.
set -euo pipefail
cd "$(dirname "$0")/.."

label=$1 out=$2
shift 2
pargs=() dargs=("--dart-define=LABEL=$label")
while [[ $# -gt 0 && $1 != -- ]]; do pargs+=("--android-project-arg=$1"); shift; done
[[ ${1:-} == -- ]] && shift
for d in "$@"; do dargs+=("--dart-define=$d"); done

ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
PKG=dev.webmtexture.spike_c
mkdir -p "$out"
rm -f "$out/$label".*

fvm flutter build apk --profile ${pargs[@]+"${pargs[@]}"} "${dargs[@]}" >"$out/$label.build.log" 2>&1 \
  || { tail -30 "$out/$label.build.log"; exit 1; }
"$ADB" install -r build/app/outputs/flutter-apk/app-profile.apk >/dev/null
"$ADB" shell am force-stop "$PKG"
sleep 3 # let the emulator settle after install
"$ADB" logcat -c

# Host-side CPU and energy impact of the emulator process, every 5 s.
qpid=$(pgrep -f "qemu-system.*-avd" | head -1)
top -l 0 -s 5 -pid "$qpid" -stats pid,cpu,power >"$out/$label.hosttop.txt" 2>&1 &
toppid=$!
trap 'kill $toppid 2>/dev/null || true' EXIT

$AD open "$PKG" --relaunch >/dev/null 2>&1 || { sleep 2; $AD open "$PKG" --relaunch >/dev/null; }
sleep 25
$AD screenshot "$out/$label.png" >/dev/null
"$ADB" shell dumpsys meminfo "$PKG" >"$out/$label.meminfo.mid.txt"
for _ in $(seq 1 120); do
  "$ADB" logcat -d | grep -q -E "SPIKE_C_RESULT ([0-9]+)/\1 |SPIKE_C_ERROR|FATAL EXCEPTION" && break
  sleep 2
done
kill $toppid 2>/dev/null || true
"$ADB" shell dumpsys meminfo "$PKG" >"$out/$label.meminfo.txt"
"$ADB" logcat -d >"$out/$label.log"
python3 -I tool/collect.py "$label" "$out" | tee -a "$out/results.jsonl"
