#!/usr/bin/env bash
# examples/151_perfViewer/tools/liveCheck.sh — a program streams into the viewer: the viewer listens
# on a free port (RAE_PERF_LISTEN=0) and follows the first program (RAE_PERF_FOLLOW=1), then
# checks/perfHud runs with RAE_PERF_LIVE pointing at it (its perf HUD's capture streams through
# lib/perf/PerfLive). The viewer ends when that program has ended (RAE_PERF_END_AFTER_LIVE=1) and
# its last lines must name the program, its ended link and the views filled from what it sent.
#
#   bash examples/151_perfViewer/tools/liveCheck.sh              # builds both with compiler/bin/rae
#   bash examples/151_perfViewer/tools/liveCheck.sh <viewer app> # a viewer already built (the gate)
set -uo pipefail
cd "$(dirname "$0")/../../.."
RAE="${RAE_BIN:-compiler/bin/rae}"
TMP="$(mktemp -d -t rae_perflive_XXXXXX)"
trap 'rm -rf "$TMP"; [ -n "${VIEWER:-}" ] && kill "$VIEWER" 2>/dev/null' EXIT
if [ $# -ge 1 ]; then
  viewer=("$1")
else
  viewer=("$RAE" run examples/151_perfViewer/Main.rae)
fi
(RAE_PERF_LISTEN=0 RAE_PERF_FOLLOW=1 RAE_PERF_END_AFTER_LIVE=1 RAE_PERF_VIEW=timeline \
  RAE_SDL_HEADLESS_MS=90000 perl -e 'alarm shift; exec @ARGV' 150 "${viewer[@]}" > "$TMP/viewer.log" 2>&1) &
VIEWER=$!
port=""
for _ in $(seq 1 120); do
  port="$(sed -n 's/.*\[perfViewer\] listening on port \([0-9]*\).*/\1/p' "$TMP/viewer.log" | head -1)"
  [ -n "$port" ] && break
  kill -0 "$VIEWER" 2>/dev/null || break
  sleep 1
done
if [ -z "$port" ]; then
  echo "FAIL: the viewer is not listening"; tail -8 "$TMP/viewer.log"; exit 1
fi
RAE_PERF_LIVE="127.0.0.1:$port" RAE_SDL_HEADLESS_MS=4000 \
  perl -e 'alarm shift; exec @ARGV' 150 "$RAE" run --target compiled --project checks/perfHud checks/perfHud/Main.rae \
  > "$TMP/program.log" 2>&1
for _ in $(seq 1 60); do
  kill -0 "$VIEWER" 2>/dev/null || break
  sleep 1
done
kill "$VIEWER" 2>/dev/null
VIEWER=""
if grep -q "profile: live link to 127.0.0.1:$port" "$TMP/program.log" \
   && grep -q "PERF HUD OK" "$TMP/program.log" \
   && grep -q "\[perfViewer\] sources: 1 live; checks/perfHud/Main.rae on .*: true blocks, ended true" "$TMP/viewer.log" \
   && grep -Eq "\[perfViewer\] timeline: [1-9][0-9]* frames, [1-9][0-9]* spans" "$TMP/viewer.log"; then
  grep "\[perfViewer\] \(sources\|timeline\)" "$TMP/viewer.log"
  echo "LIVE OK"
  exit 0
fi
echo "FAIL: the program did not stream into the viewer"
tail -6 "$TMP/program.log"; grep "perfViewer" "$TMP/viewer.log" | tail -6
exit 1
