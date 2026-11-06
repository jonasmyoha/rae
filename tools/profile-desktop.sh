#!/usr/bin/env bash
# Profile an app on this machine with one command, one run per place, and print the same
# summary as a phone's (tools/ios/profile-ios.sh, tools/profile-summary.py), so a comparison
# can start here. It starts each run fresh and uncapped (RAE_FRESH_START, RAE_PRESENT_MODE=
# immediate), with Rae's GPU pass and frame timing on, and opens the app's window.
#
#   tools/profile-desktop.sh <Main.rae> [seconds per place]
#
#   PLACES          "name|x y" lines: where each run starts; AT_ENV names the app's environment
#                   variable that starts it at "x y". Default: one run, "here|".
#   EXTRA_ENV       more of the environment, as NAME=value words
#   SUMMARY_FIRST, SUMMARY_MIDDLE   as in tools/ios/profile-ios.sh
#   OUT             logs and the summary (/tmp/desktop-profile)
#   RAE_BIN         the compiler (this checkout's compiler/bin/rae)
#   BUILD_SECONDS   how long a run may take to start (300)
#   TOTAL_SECONDS   the limit for the whole profile (1800)
# It stops at the first run that fails or at the time limit, says so, summarizes what it has and
# exits 1. Medians skip each run's first 15 s, so give a place at least 30 s. With RAE_AB in
# EXTRA_ENV the app ends by itself, and a run ends there.
set -uo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
ENTRY=${1:-}
PER=${2:-60}
if [ -z "$ENTRY" ]; then
  echo "usage: $0 <Main.rae> [seconds per place]" >&2
  exit 2
fi
OUT=${OUT:-/tmp/desktop-profile}
RAE_BIN=${RAE_BIN:-$root/compiler/bin/rae}
AT_ENV=${AT_ENV:-}
EXTRA_ENV=${EXTRA_ENV:-}
PLACES=${PLACES:-"here|"}
BUILD_SECONDS=${BUILD_SECONDS:-300}
TOTAL_SECONDS=${TOTAL_SECONDS:-1800}
mkdir -p "$OUT"
printf '%s\n' "$PLACES" > "$OUT/places.txt"
# A log of an earlier profile must not read as this one's
while IFS='|' read -r name at; do
  [ -n "$name" ] && rm -f "$OUT/$name.log"
done < "$OUT/places.txt"

stopped=""
app=0

# Ends this script's own run, and only that: TERM, which `rae run` passes on to the app
end_run() {
  [ "$app" -gt 0 ] || return 0
  kill -TERM "$app" 2> /dev/null
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 "$app" 2> /dev/null || break
    sleep 1
  done
  kill -9 "$app" 2> /dev/null
  wait "$app" 2> /dev/null
  app=0
}

# Seconds left of TOTAL_SECONDS
left() {
  echo $((TOTAL_SECONDS - SECONDS))
}

while IFS='|' read -r name at; do
  [ -z "$name" ] && continue
  [ -n "$stopped" ] && break
  echo "== $name ($at) $(date +%H:%M:%S)"
  log="$OUT/$name.log"
  # shellcheck disable=SC2086
  env RAE_GPU_PASS_TIMING=1 RAE_FRAME_TIMING=1 RAE_FRESH_START=1 RAE_PRESENT_MODE=immediate \
    ${at:+${AT_ENV:+$AT_ENV="$at"}} $EXTRA_ENV "$RAE_BIN" run "$ENTRY" > "$log" 2>&1 &
  app=$!
  # Until the build is over and the app started, or the run is over
  waited=0
  until grep -q '@@RAE_APP_START@@' "$log" 2> /dev/null; do
    if ! kill -0 "$app" 2> /dev/null; then
      wait "$app"
      code=$?
      app=0
      stopped="$name did not start (exit $code); its log is $log"
      break
    fi
    if [ "$waited" -ge "$BUILD_SECONDS" ] || [ "$(left)" -le 0 ]; then
      stopped="$name did not start within $waited s; its log is $log"
      break
    fi
    sleep 1
    waited=$((waited + 1))
  done
  if [ -n "$stopped" ]; then
    end_run
    break
  fi
  # The place's time, or until the app ends by itself, or the limit
  ran=0
  while [ "$ran" -lt "$PER" ] && kill -0 "$app" 2> /dev/null; do
    if [ "$(left)" -le 0 ]; then
      stopped="the time limit of $TOTAL_SECONDS s came during $name"
      break
    fi
    sleep 1
    ran=$((ran + 1))
  done
  if kill -0 "$app" 2> /dev/null; then
    end_run
  else
    wait "$app"
    code=$?
    app=0
    if [ "$code" -ne 0 ]; then
      stopped="$name ended early, with exit $code; its log is $log"
    fi
  fi
done < "$OUT/places.txt"

python3 "$here/profile-summary.py" "$OUT" "${SUMMARY_FIRST:-}" "${SUMMARY_MIDDLE:-}" | tee "$OUT/summary.txt"
if [ -n "$stopped" ]; then
  echo "STOPPED: $stopped" | tee -a "$OUT/summary.txt"
  exit 1
fi
