#!/usr/bin/env bash
# Profile an app on a connected iPhone, one run per place: launch it there
# with Rae's GPU pass and frame timing on (RAE_GPU_PASS_TIMING,
# RAE_FRAME_TIMING: the [gpu passes], [frame] and app3d/FrameLog [frames]
# lines), let it run, copy its log off the phone, then summarise every run:
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
#   SUMMARY_ONLY=1  no runs: summarise the logs already in OUT
# The app is closed after each run, so it does not heat the phone between
# places.
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
mkdir -p "$OUT"

# The game's process id on the phone (none when it is not running)
game_pid() {
  xcrun devicectl device info apps --device "$DEVICE" --json-output "$OUT/apps.json" > /dev/null 2>&1
  xcrun devicectl device info processes --device "$DEVICE" --json-output "$OUT/processes.json" > /dev/null 2>&1
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
  [ -n "$pid" ] && xcrun devicectl device process terminate --device "$DEVICE" --pid "$pid" > /dev/null 2>&1
}

copy_log() {
  xcrun devicectl device copy from --device "$DEVICE" --domain-type appDataContainer \
    --domain-identifier "$APP" --source Documents/rae.log --destination "$1" > /dev/null 2>&1
}
PLACES=${PLACES:-"here|"}
if [ "$SUMMARY_ONLY" != 1 ]; then
printf '%s\n' "$PLACES" > "$OUT/places.txt"
while IFS='|' read -r name at; do
  [ -z "$name" ] && continue
  env='{"RAE_GPU_PASS_TIMING":"1","RAE_FRAME_TIMING":"1"'
  [ -n "$EXTRA_ENV" ] && env="$env,$EXTRA_ENV"
  [ -n "$at" ] && [ -n "$AT_ENV" ] && env="$env,\"$AT_ENV\":\"$at\""
  env="$env}"
  echo "== $name ($at) $(date +%H:%M:%S)"
  tries=0
  launched=0
  while :; do
    if ! xcrun devicectl device process launch --device "$DEVICE" --terminate-existing -e "$env" "$APP" > "$OUT/$name.launch" 2>&1; then
      echo "launch failed:"; cat "$OUT/$name.launch"; break
    fi
    launched=1
    [ "$COOL_WAIT" -le 0 ] && break
    sleep 10
    copy_log "$OUT/$name.log"
    thermal=$(grep -m1 '^\[frames\]' "$OUT/$name.log" 2>/dev/null | sed -E 's/.*thermal ([0-9]).*/\1/')
    echo "   thermal at start: ${thermal:-unknown}"
    if [ "${thermal:-1}" = "0" ] || [ "$tries" -ge "$COOL_TRIES" ]; then break; fi
    tries=$((tries + 1))
    stop_game
    echo "   warm; closed, cooling ${COOL_WAIT} s (try $tries of $COOL_TRIES)"
    sleep "$COOL_WAIT"
  done
  [ "$launched" = 1 ] || continue
  sleep "$PER"
  copy_log "$OUT/$name.log" || echo "copy failed"
  stop_game
done < "$OUT/places.txt"
fi
# The summary: medians over each run after its first 15 s (loading)
python3 - "$OUT" "$SUMMARY_FIRST" "$SUMMARY_MIDDLE" <<'PY' | tee "$OUT/summary.txt"
import re, sys, statistics as st
out = sys.argv[1]
# "[prefix]|text" lines: (the prefix, the text a line must hold, its label)
def kinds(spec):
    found = []
    for line in spec.splitlines():
        if '|' not in line: continue
        prefix, text = line.split('|', 1)
        found.append((prefix, text, prefix.strip('[]')))
    return found
firsts, middles = kinds(sys.argv[2]), kinds(sys.argv[3])
def med(xs): return st.median(xs) if xs else float('nan')
for line in open(f'{out}/places.txt'):
    name = line.split('|')[0].strip()
    if not name: continue
    try: lines = open(f'{out}/{name}.log').read().splitlines()
    except FileNotFoundError: print(name, 'no log'); continue
    frames = [l for l in lines if l.startswith('[frames]')][3:]
    gpu = [l for l in lines if l.startswith('[gpu passes]')][3:]
    timing = [l for l in lines if l.startswith('[frame] ') and 'in flight' in l][2:]
    if not frames: print(name, 'no [frames] lines'); continue
    fm = [float(re.search(r'median ([\d.]+)', l).group(1)) for l in frames]
    fp = [float(re.search(r'p95 ([\d.]+)', l).group(1)) for l in frames]
    over = sum(int(re.search(r'(\d+) over', l).group(1)) for l in frames)
    count = sum(int(re.search(r'\] (\d+) in', l).group(1)) for l in frames)
    scales = sorted(set(re.search(r'scale ([\d.]+)', l).group(1) for l in frames))
    thermal = sorted(set(re.search(r'thermal (\d)', l).group(1) for l in frames))
    passes = {}
    for l in gpu:
        for k, v in re.findall(r'([a-z][a-z0-9 :()/,A-Z-]*?) (\d+)(?: \||$)', l.split('us: ', 1)[1]):
            passes.setdefault(k.strip(), []).append(int(v))
    pm = {k: med(v) / 1000 for k, v in passes.items()}
    # The GPU's idle time lands in the antialias slot (rendering-plan.md)
    busy = pm.get('total', 0) - pm.get('antialias (SMAA or FXAA)', 0)
    print(f'## {name}: {count} frames, median {med(fm)} ms, p95 {med(fp)} ms, {over} over budget, scale {scales}, thermal {thermal}')
    print('   gpu ms: ' + ', '.join(f'{k} {v:.2f}' for k, v in pm.items() if k != 'total') + f'; busy (total - antialias slot) {busy:.2f}')
    if timing:
        cpu = [float(re.search(r'cpu ([\d.]+)', l).group(1)) for l in timing]
        acquire = [float(re.search(r'acquire ([\d.]+)', l).group(1)) for l in timing]
        print(f'   cpu {med(cpu):.1f} ms (max {max(cpu):.1f}), waiting for the drawable {med(acquire):.1f} ms')
    for prefix, text, label in firsts:
        found = [l for l in lines if l.startswith(prefix) and text in l]
        if found:
            print(f'   {label} at start:', found[0].split('] ', 1)[1])
    for prefix, text, label in middles:
        found = [l for l in lines if l.startswith(prefix) and text in l]
        if found:
            print(f'   {label}:', found[len(found) // 2].split('] ', 1)[1])
PY
