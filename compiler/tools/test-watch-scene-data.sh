#!/bin/bash
# test-watch-scene-data.sh — `rae watch` restarts the app when a `.raescene`
# changes, not only when a `.rae` source does. A scene is DATA the running app
# loads, so no build sees it; but `rae watch` is the one dev loop ("edit
# anything, see it"), and an app must never need its own FileWatch over its
# own scenes to get scene edits reflected under the supervisor.
#
# Runs the supervisor on a short-lived CLI program that prints the scene file's
# content (a clean exit inside the health window keeps the supervisor alive,
# monitoring sources), rewrites the `.raescene`, and expects the app to be
# re-run and print the NEW content. Prints one PASS/FAIL line per check; exits
# non-zero if any failed. Run standalone (from compiler/) or from
# run_tests_parallel.sh at the end of a full run.
set -u
BIN="${RAE_BIN:-bin/rae}"
[ -x "$BIN" ] || BIN="./bin/rae"
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "PASS: watch-scene-data/$1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL: watch-scene-data/$1 — $2"; }

WORK=$(mktemp -d)
WATCH_PID=""
cleanup() {
  if [ -n "$WATCH_PID" ]; then kill "$WATCH_PID" 2>/dev/null; wait "$WATCH_PID" 2>/dev/null; fi
  rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$WORK/assets/scenes"
printf '{ "title": "before" }\n' > "$WORK/assets/scenes/Menu.raescene"
cat > "$WORK/Main.rae" <<'RAE'
import Files

func main() {
  if let source: String = Files.readFile(path: "assets/scenes/Menu.raescene") {
    log("scene: {source.trim()}")
  } else {
    log("scene: missing")
  }
}
RAE

LOG="$WORK/watch.log"
( cd "$WORK" && RAE_FORMAT=off "$BIN" watch Main.rae > "$LOG" 2>&1 ) &
WATCH_PID=$!

wait_for() {  # wait_for <pattern> <seconds>
  local i=0
  while [ "$i" -lt "$(( $2 * 5 ))" ]; do
    grep -q "$1" "$LOG" 2>/dev/null && return 0
    kill -0 "$WATCH_PID" 2>/dev/null || return 1
    sleep 0.2; i=$((i+1))
  done
  return 1
}

if wait_for 'scene: { "title": "before" }' 60; then ok "initial run prints the scene"
else bad "initial run prints the scene" "no initial output in 60 s; log: $(tail -5 "$LOG" | tr '\n' '|')"; fi

# The watcher polls mtimes at 1 s granularity: make sure the rewrite lands on a
# later second than the snapshot, then change the file.
sleep 1.2
printf '{ "title": "after" }\n' > "$WORK/assets/scenes/Menu.raescene"

if wait_for 'change in .*Menu.raescene' 30; then ok "scene edit is a watched change"
else bad "scene edit is a watched change" "no 'change in' line in 30 s; log: $(tail -5 "$LOG" | tr '\n' '|')"; fi

if wait_for 'scene: { "title": "after" }' 60; then ok "app re-runs with the new scene data"
else bad "app re-runs with the new scene data" "new content never printed; log: $(tail -8 "$LOG" | tr '\n' '|')"; fi

echo "watch-scene-data: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
