#!/usr/bin/env bash
# Runs the libFuzzer targets in parallel for a fixed time, each with its seed
# corpora and dictionary. Used by CI (60 s on PRs, 30 min nightly) and
# locally (T1.7 acceptance: 600 s).
#
#   tool/fuzz.sh <seconds> [fuzz_sniff fuzz_probe fuzz_webm fuzz_webp fuzz_gif fuzz_wsc]
#
# Needs a fuzz build first:
#   cmake -S . -B build/fuzz -DWT_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/fuzz -j
#
# Outputs, under build/fuzz/ (override with WT_FUZZ_DIR):
#   corpus/<target>/     new inputs; reused by the next run
#   artifacts/<target>/  crash-*, timeout-*, oom-* reproducers
#   logs/<target>.log
# Exits non-zero if any target finds something. Reproduce a finding with
#   build/fuzz/<target> build/fuzz/artifacts/<target>/crash-...
# then fix it and add the input to fuzz/corpus/<corpus>/ as a regression seed.
set -euo pipefail

cd "$(dirname "$0")/.."
seconds=${1:?usage: tool/fuzz.sh <seconds> [target...]}
shift
targets=("$@")
[[ ${#targets[@]} -gt 0 ]] || targets=(fuzz_sniff fuzz_probe fuzz_webm fuzz_webp fuzz_gif fuzz_wsc)
dir=${WT_FUZZ_DIR:-build/fuzz}

# Seed directories (read-only; libFuzzer writes into the first corpus dir,
# which is the scratch one) and extra flags per target.
seeds() {
  case $1 in
    fuzz_sniff) echo "test_assets" ;;
    fuzz_probe) echo "test_assets fuzz/corpus/webm fuzz/corpus/webp fuzz/corpus/gif" ;;
    fuzz_webm) echo "fuzz/corpus/webm" ;;
    fuzz_webp) echo "test_assets fuzz/corpus/webp" ;;
    fuzz_gif) echo "test_assets fuzz/corpus/gif" ;;
    fuzz_wsc) echo "fuzz/corpus/wsc" ;;
    *) echo "unknown target: $1" >&2; return 1 ;;
  esac
}
flags() {
  case $1 in
    fuzz_webm) echo "-dict=fuzz/webm.dict" ;;
    fuzz_webp) echo "-dict=fuzz/webp.dict" ;;
    fuzz_gif) echo "-dict=fuzz/gif.dict" ;;
    fuzz_wsc) echo "-dict=fuzz/wsc.dict" ;;
  esac
}

pids=()
for t in "${targets[@]}"; do
  [[ -x $dir/$t ]] || { echo "$dir/$t not built" >&2; exit 1; }
  mkdir -p "$dir/corpus/$t" "$dir/artifacts/$t" "$dir/logs"
  # -timeout: a single input taking 10 s is a hang worth reporting.
  # -malloc_limit_mb: the harnesses decode at most 256 x 256 (WebM: 1024 x
  # 1024 for the index only), so one allocation over 256 MB is a bomb, like
  # VP8's frame buffers at a 16383 x 16383 keyframe size (T6.1, S-01).
  # shellcheck disable=SC2046
  "$dir/$t" -max_total_time="$seconds" -timeout=10 -malloc_limit_mb=256 \
    -print_final_stats=1 \
    -artifact_prefix="$dir/artifacts/$t/" $(flags "$t") \
    "$dir/corpus/$t" $(seeds "$t") >"$dir/logs/$t.log" 2>&1 &
  pids+=($!)
done

status=0
for i in "${!targets[@]}"; do
  t=${targets[$i]}
  if wait "${pids[$i]}"; then
    runs=$(sed -n 's/^stat::number_of_executed_units: *//p' "$dir/logs/$t.log")
    cov=$(grep -Eo 'cov: [0-9]+' "$dir/logs/$t.log" | tail -1)
    echo "$t: OK, ${runs:-?} runs in ${seconds}s, ${cov:-cov: ?}"
  else
    status=1
    echo "$t: FAILED, see $dir/logs/$t.log and $dir/artifacts/$t/" >&2
    grep -E '^(==[0-9]+==ERROR|SUMMARY|.*runtime error)' "$dir/logs/$t.log" >&2 || tail -20 "$dir/logs/$t.log" >&2
  fi
done
exit $status
