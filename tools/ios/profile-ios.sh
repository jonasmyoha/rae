#!/usr/bin/env bash
# Profile an app on a connected iPhone, one run per place: launch it there
# with Rae's GPU pass and frame timing on (RAE_GPU_PASS_TIMING,
# RAE_FRAME_TIMING: the [gpu passes], [frame] and app3d/FrameLog [frames]
# lines), let it run, copy its log off the phone, then summarize every run:
# its frames' median and 95th percentile, the frames over budget, the scales
# and thermal states, the GPU passes, the CPU time. Install the current build
# first (tools/ios/run-ios.sh).
#
#   APP=<bundle id> tools/ios/profile-ios.sh <device id> [seconds per place]
#
#   APP             the app's bundle id (required)
#   PLACES          "name|x y" lines: where each run starts; AT_ENV names the
#                   app's environment variable that starts it at "x y" (an
#                   empty x y starts it where it starts). Default: one run,
#                   "here|".
#   EXTRA_ENV       more of the launch environment, as JSON members
#                   ('"NAME":"value",...')
#   SUMMARY_FIRST   "[prefix]|text" lines: the summary also shows the first
#                   log line with that prefix and text ("<prefix> at start")
#   SUMMARY_MIDDLE  the same, its middle line ("<prefix>")
#   OUT             logs and the summary (/tmp/ios-profile)
#   COOL_WAIT=<s>   start each place only on a cool phone: 10 s into a launch
#                   it reads the thermal state of the first [frames] line, and
#                   if it is above 0 closes the app, waits COOL_WAIT and tries
#                   again (COOL_TRIES times, default 6; past that the place
#                   runs as it is, and its summary says so)
#   TOTAL_SECONDS   the limit for the whole profile (1800)
#   SUMMARY_ONLY=1  no runs: summarize the logs already in OUT
# It stops at the first launch or log copy that fails, or at the time limit, says so with a
# STOPPED line, summarizes what it has and exits 1: a locked phone fails the same way at every
# place. The app is asked to end after each run, so it does not heat the phone between places.
# An app may stay on the screen (tools/ios/README.md), so a warning says when it is still
# running; the next launch ends it. tools/profile-desktop.sh gives the same summary on a desktop.
set -uo pipefail
DEVICE=${1:-}
PER=${2:-80}
OUT=${OUT:-/tmp/ios-profile}
APP=${APP:-}
AT_ENV=${AT_ENV:-}
EXTRA_ENV=${EXTRA_ENV:-}
SUMMARY_FIRST=${SUMMARY_FIRST:-}
SUMMARY_MIDDLE=${SUMMARY_MIDDLE:-}
SUMMARY_ONLY=${SUMMARY_ONLY:-0}
if [ "$SUMMARY_ONLY" != 1 ] && { [ -z "$APP" ] || [ -z "$DEVICE" ]; }; then
  echo "usage: APP=<bundle id> $0 <device id> [seconds per place]" >&2
  exit 2
fi
XCODE=$(ls -d /Applications/Xcode*.app 2>/dev/null | sort | tail -1)
export DEVELOPER_DIR=${DEVELOPER_DIR:-$XCODE/Contents/Developer}
COOL_WAIT=${COOL_WAIT:-0}
COOL_TRIES=${COOL_TRIES:-6}
TOTAL_SECONDS=${TOTAL_SECONDS:-1800}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$OUT"
stopped=""

# Seconds left of TOTAL_SECONDS
left() {
  echo $((TOTAL_SECONDS - SECONDS))
}

# One device call, which may hang on a phone gone away: at most 60 s
device() {
  perl -e 'alarm shift; exec @ARGV' 60 xcrun devicectl device "$@"
}

# Sleeps $1 s; false, and nothing more, if the time limit comes first
wait_within() {
  if [ "$1" -ge "$(left)" ]; then
    sleep "$(( $(left) > 0 ? $(left) : 0 ))"
    return 1
  fi
  sleep "$1"
}

# The game's process id on the phone (none when it is not running)
game_pid() {
  device info apps --device "$DEVICE" --json-output "$OUT/apps.json" > /dev/null 2>&1 || return 0
  device info processes --device "$DEVICE" --json-output "$OUT/processes.json" > /dev/null 2>&1 || return 0
  python3 - "$OUT" "$APP" <<'PY'
import json, sys
out, app = sys.argv[1], sys.argv[2]
url = next((a['url'] for a in json.load(open(f'{out}/apps.json'))['result']['apps'] if a['bundleIdentifier'] == app), None)
for p in json.load(open(f'{out}/processes.json'))['result']['runningProcesses']:
    if url and p.get('executable', '').startswith(url):
        print(p['processIdentifier'])
PY
}

stop_game() {
  local pid
  pid=$(game_pid)
  [ -n "$pid" ] || return 0
  device process terminate --device "$DEVICE" --pid "$pid" > /dev/null 2>&1
  sleep 1
  [ -z "$(game_pid)" ] || echo "   warning: the app is still running; the next launch ends it"
}

# Copy the app's log to $1. A copy over Wi-Fi fails now and then and works a few seconds
# later, so a failed copy is tried two more times, 5 s apart. devicectl's output is kept in
# $1.copy.txt; when all three fail its ERROR line (else its last line) is printed, so a
# STOPPED line says why.
copy_log() {
  local try
  for try in 1 2 3; do
    if device copy from --device "$DEVICE" --domain-type appDataContainer \
      --domain-identifier "$APP" --source Documents/rae.log --destination "$1" > "$1.copy.txt" 2>&1; then
      [ "$try" -gt 1 ] && echo "   the log copied on try $try"
      return 0
    fi
    [ "$try" -lt 3 ] && sleep 5
  done
  local reason
  reason=$(grep -m1 '^ERROR:' "$1.copy.txt")
  [ -n "$reason" ] || reason=$(grep -v '^[[:space:]]*$' "$1.copy.txt" | tail -1)
  echo "   devicectl, three tries: $reason"
  return 1
}
PLACES=${PLACES:-"here|"}
if [ "$SUMMARY_ONLY" != 1 ]; then
printf '%s\n' "$PLACES" > "$OUT/places.txt"
# A log of an earlier profile must not read as this one's
while IFS='|' read -r name at; do
  [ -n "$name" ] && rm -f "$OUT/$name.log"
done < "$OUT/places.txt"
while IFS='|' read -r name at; do
  [ -z "$name" ] && continue
  env='{"RAE_GPU_PASS_TIMING":"1","RAE_FRAME_TIMING":"1"'
  [ -n "$EXTRA_ENV" ] && env="$env,$EXTRA_ENV"
  [ -n "$at" ] && [ -n "$AT_ENV" ] && env="$env,\"$AT_ENV\":\"$at\""
  env="$env}"
  echo "== $name ($at) $(date +%H:%M:%S)"
  tries=0
  while :; do
    if ! device process launch --device "$DEVICE" --terminate-existing -e "$env" "$APP" > "$OUT/$name.launch" 2>&1; then
      cat "$OUT/$name.launch"
      stopped="the launch at $name failed (is the phone locked?)"
      break
    fi
    [ "$COOL_WAIT" -le 0 ] && break
    if ! wait_within 10; then
      stopped="the time limit of $TOTAL_SECONDS s came during $name"
      break
    fi
    if ! copy_log "$OUT/$name.log"; then
      stopped="copying the log at $name failed"
      break
    fi
    thermal=$(grep -m1 '^\[frames\]' "$OUT/$name.log" 2>/dev/null | sed -E 's/.*thermal ([0-9]).*/\1/')
    echo "   thermal at start: ${thermal:-unknown}"
    if [ "${thermal:-1}" = "0" ] || [ "$tries" -ge "$COOL_TRIES" ]; then break; fi
    tries=$((tries + 1))
    stop_game
    echo "   warm; closed, cooling ${COOL_WAIT} s (try $tries of $COOL_TRIES)"
    if ! wait_within "$COOL_WAIT"; then
      stopped="the time limit of $TOTAL_SECONDS s came during $name"
      break
    fi
  done
  if [ -z "$stopped" ]; then
    wait_within "$PER" || stopped="the time limit of $TOTAL_SECONDS s came during $name"
    if ! copy_log "$OUT/$name.log" && [ -z "$stopped" ]; then
      stopped="copying the log at $name failed"
    fi
  fi
  stop_game
  [ -n "$stopped" ] && break
done < "$OUT/places.txt"
fi
python3 "$here/../profile-summary.py" "$OUT" "$SUMMARY_FIRST" "$SUMMARY_MIDDLE" | tee "$OUT/summary.txt"
if [ -n "$stopped" ]; then
  echo "STOPPED: $stopped" | tee -a "$OUT/summary.txt"
  exit 1
fi
