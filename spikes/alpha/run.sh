#!/usr/bin/env bash
# Spike A (T0.7): builds the spike CLI and runs it on the corpus.
#
#   spikes/alpha/run.sh            release build: verify + experiments + sizes
#   spikes/alpha/run.sh --asan N   ASan/UBSan build: verify + experiments +
#                                  N stress iterations (default 20000)
#
# Run spikes/alpha/build-host-deps.sh once first.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ASSETS="$ROOT/test_assets"
MODE="${1:-release}"
OUT="$ROOT/build/spike-alpha"
CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release)
if [ "$MODE" = "--asan" ]; then
  OUT="$ROOT/build/spike-alpha-asan"
  CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Debug -DSPIKE_SANITIZE=ON)
fi

cmake -S "$ROOT/spikes/alpha" -B "$OUT" "${CMAKE_ARGS[@]}" >/dev/null
cmake --build "$OUT" -j >/dev/null

# manifest.json flattened to one '|'-separated line per file.
EXPECT="$OUT/expect.txt"
jq -r '.files[] | [.name, .kind, .expect, (.codec // ""), (.width // 0),
    (.height // 0), (.alpha // false), (.has_audio // false),
    (.frame_count // 0), ((.pts_ms // []) | join(",")),
    ((.keyframes // []) | join(",")), ((.ref_frames // []) | join(","))]
    | map(tostring) | join("|")' "$ASSETS/manifest.json" >"$EXPECT"

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:allocator_may_return_null=0}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"

"$OUT/alpha_spike" verify "$ASSETS" "$EXPECT"
echo
"$OUT/alpha_spike" experiments "$ASSETS"

if [ "$MODE" = "--asan" ]; then
  echo
  "$OUT/alpha_spike" stress "$ASSETS" "${2:-20000}" 1
else
  echo
  echo "## Code size (-Os, dead-stripped, $(uname -m); bytes over the no-demuxer baseline)"
  echo
  text() { size "$1" | awk 'NR==2 { print $1 }'; }
  if [ "$(uname -s)" = Darwin ]; then
    text() { size -m "$1" | awk '/Section __text/ { print $NF; exit }'; }
  fi
  base="$(text "$OUT/size_none")"
  for p in libwebm mini; do
    t="$(text "$OUT/size_$p")"
    echo "- $p: __text +$((t - base)) bytes"
  done
fi
