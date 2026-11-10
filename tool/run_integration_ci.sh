#!/usr/bin/env bash
# Runs one integration test file on a device for CI, with retries.
#
#   tool/run_integration_ci.sh <device-id> <test file>   (from example/)
#
# On GitHub's macOS runners the iOS simulator sometimes never starts the app,
# fails to start the Dart Development Service, or leaves `flutter test` hanging
# after the last test has passed. So the verdict comes from the test output
# rather than the exit code: "All tests passed!" ends an attempt as a pass, a
# failure summary or the per-attempt limit as a fail, and then it retries.
set -u
device=$1
file=$2
attempts=${ATTEMPTS:-3}
limit=${ATTEMPT_LIMIT_SECONDS:-420}
log=$(mktemp)

for attempt in $(seq "$attempts"); do
  echo "::group::attempt $attempt of $attempts: $file"
  fvm flutter test "$file" -d "$device" --reporter expanded >"$log" 2>&1 &
  pid=$!
  start=$SECONDS
  verdict=
  while kill -0 "$pid" 2>/dev/null; do
    if grep -q "All tests passed!" "$log"; then
      verdict=pass
      break
    elif grep -q "Some tests failed\." "$log"; then
      verdict=fail
      break
    elif ((SECONDS - start > limit)); then
      verdict=timeout
      break
    fi
    sleep 5
  done
  if [ -z "$verdict" ]; then
    # Exited by itself, possibly just after the last line was written.
    if wait "$pid" || grep -q "All tests passed!" "$log"; then
      verdict=pass
    else
      verdict=fail
    fi
  fi
  # Stop whatever is left (flutter test, xcodebuild, the test runner).
  pkill -P "$pid" 2>/dev/null
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null

  cat "$log"
  echo "::endgroup::"
  echo "attempt $attempt: $verdict after $((SECONDS - start))s"
  if [ "$verdict" = pass ]; then
    exit 0
  fi
done
echo "::error::$file failed $attempts attempts"
exit 1
