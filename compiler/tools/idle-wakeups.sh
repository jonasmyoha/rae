#!/bin/bash
# Wakeups of a windowed app, idle and visible, then hidden (docs/ui-render-loop-performance.md).
#
# The number that matters is how often the app's main loop runs: the runtime
# logs one line per event poll under RAE_LOOP_TRACE=1 and this counts them. An
# idle UI app should block in its event wait, so that is ~0-2/s visible and
# ~1/s hidden; a number near 20 or 60 is a timer someone forgot (a watcher
# poll, a one-frame wait for an effect nothing draws). The process's context
# switches (top's csw; its idlew column is unreliable on macOS) are printed
# too, but Metal, wgpu and the display link keep their own threads switching
# while the loop is blocked, so they have a floor of tens per second.
#
# usage: compiler/tools/idle-wakeups.sh <project-dir> <entry.rae> [settle-sec] [measure-sec]
#   Extra environment (a scene to open, ...) is passed through to the app.
#   Prints "visible-idle loop N/s ..." and "hidden loop N/s ..."; exit 0. macOS only (top csw,
#   System Events to hide the app). Opens a real window and briefly takes focus.
#
# Run from the repo root. The app is started with `rae run`, so the first run
# of an example includes its build.
set -u
PROJECT="${1:?project dir}"
ENTRY="${2:?entry .rae}"
SETTLE="${3:-6}"
MEASURE="${4:-10}"
LOG="$(mktemp -t idle-wakeups)"

# The app binary, by executable name: the C compiler's command line names the
# same file, so `pgrep -f` would find the build instead.
app_pids() { ps -axo pid=,comm= | awk '$2 ~ /rae_compiled_[0-9]+\.bin$/ { print $1 }'; }
before="$(app_pids | sort)"
RAE_LOOP_TRACE=1 perl -e 'alarm shift; exec @ARGV' 600 ./compiler/bin/rae run --target compiled \
  --project "$PROJECT" "$ENTRY" > "$LOG" 2>&1 &
RUNNER=$!

APP=""
for _ in $(seq 1 300); do
  for pid in $(app_pids); do
    if ! echo "$before" | grep -qx "$pid"; then APP="$pid"; fi
  done
  [ -n "$APP" ] && break
  kill -0 "$RUNNER" 2>/dev/null || break
  sleep 1
done
if [ -z "$APP" ]; then
  echo "idle-wakeups: the app did not start (log: $LOG)"
  tail -20 "$LOG"
  exit 1
fi

# Cumulative context switches of the process (top prints "<pid> <csw>").
csw() { top -l 1 -pid "$1" -stats pid,csw 2>/dev/null | awk -v p="$1" '$1 == p { gsub(/[^0-9]/, "", $2); print $2 }'; }
now_ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time() * 1000'; }
# Loop iterations come from the runtime's RAE_LOOP_TRACE lines ("[loop-wake]
# <epoch ms>", one per event poll) inside the measured window.
measure() {
  local a b start end loops
  a="$(csw "$APP")"
  start="$(now_ms)"
  sleep "$MEASURE"
  end="$(now_ms)"
  b="$(csw "$APP")"
  loops="$(awk -v s="$start" -v e="$end" '$1 == "[loop-wake]" && $2 >= s && $2 < e { n++ } END { print n + 0 }' "$LOG")"
  echo "$1 loop $(( loops / MEASURE ))/s (process csw $(( (b - a) / MEASURE ))/s)"
}
app_script() {
  perl -e 'alarm shift; exec @ARGV' 15 osascript -e \
    "tell application \"System Events\" to tell (first process whose unix id is $APP) to $1" \
    > /dev/null 2>&1
}

sleep "$SETTLE"
kill -0 "$APP" 2>/dev/null || { echo "idle-wakeups: the app exited (log: $LOG)"; tail -20 "$LOG"; exit 1; }
app_script "set frontmost to true"
sleep 2
measure "visible-idle"
app_script "set visible to false"
sleep 2
measure "hidden"
app_script "set visible to true"

kill "$APP" 2>/dev/null
sleep 1
kill "$RUNNER" 2>/dev/null
rm -f "$LOG"
exit 0
