#!/usr/bin/env bash
# Windowed hover soak for the UI editor: the same question as hover-soak.sh
# (does hovering leak?), but in a VISIBLE window driven by the REAL mouse, so
# the run goes through real SDL motion events and you can watch it. Builds the
# editor and mouse-sweep.c, opens MainMenu, and sweeps the cursor down the
# window's centre column every SWEEP seconds (default 4) for SECONDS (default
# 300), then quits the app with Cmd+Q.
#
#   bash examples/121_ui_editor/tools/hover-soak-window.sh [SECONDS] [SWEEP]
#
# It reports three independent signals:
#   - resident memory every 10 s;
#   - `heap` snapshots at 60 s and near the end — total and plain-malloc
#     (`non-object`, where Rae's allocations live) block counts. This is the
#     check that catches memory piling up in a list that is still in use, which
#     the exit counters and `leaks` both miss;
#   - RAE_MEM_STATS outstanding String/buffer counts at exit.
# Report only: it does not fail on growth. The terminal running it needs
# Accessibility permission to move the mouse. Leave the mouse alone meanwhile.
set -euo pipefail
SECONDS_TO_RUN="${1:-300}"
SWEEP="${2:-4}"
REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$REPO_ROOT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
clang -O2 -o "$TMP/mouse-sweep" examples/121_ui_editor/tools/mouse-sweep.c \
  -framework ApplicationServices
compiler/bin/rae build --target compiled --emit-c --project examples/121_ui_editor \
  --out "$TMP/out.c" examples/121_ui_editor/Main.rae > "$TMP/emit.log" 2>&1 \
  || { echo "emit failed:"; tail -20 "$TMP/emit.log"; exit 1; }
WGPU="${WGPU_NATIVE:-$HOME/.local/wgpu-native}"
gcc -O2 -w -o "$TMP/app" "$TMP/out.c" "$TMP/rae_runtime.c" -I"$TMP" \
  -I/opt/homebrew/include -L/opt/homebrew/lib -DRAE_HAS_SDL3 -DRAE_HAS_WEBGPU \
  -I"$WGPU/include" -L"$WGPU/lib" -lwgpu_native -Wl,-rpath,"$WGPU/lib" \
  -framework Metal -framework QuartzCore -lSDL3 -framework Foundation \
  -framework ImageIO -framework CoreGraphics > "$TMP/link.log" 2>&1 \
  || { echo "link failed:"; tail -20 "$TMP/link.log"; exit 1; }
env RAE_UI_EDITOR_SCENE=examples/121_ui_editor/assets/samples/MainMenu.raescene \
  RAE_UI_EDITOR_ROOT=examples/121_ui_editor/assets/samples RAE_MEM_STATS=1 \
  perl -e 'alarm shift; exec @ARGV' $((SECONDS_TO_RUN + 120)) "$TMP/app" > "$TMP/run.log" 2>&1 &
PID=$!
"$TMP/mouse-sweep" "$PID" "$SECONDS_TO_RUN" "$SWEEP" &
DRIVER=$!
heapLine() {
  heap "$PID" 2>/dev/null | awk '/All zones: .* bytes\)/ {print "total", $3, "blocks", $5, "bytes"}
    / non-object *$/ {print "malloc", $1, "blocks", $2, "bytes"}' | tr '\n' ' ' | tr -d '('
}
echo "seconds rss_kb"
LATE=$((SECONDS_TO_RUN - 30))
for t in $(seq 0 10 "$SECONDS_TO_RUN"); do
  RSS=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ') || true
  [ -z "$RSS" ] && break
  echo "$t $RSS"
  [ "$t" -eq 60 ] && echo "heap@60s: $(heapLine)"
  [ "$t" -eq "$LATE" ] && [ "$LATE" -gt 60 ] && echo "heap@${LATE}s: $(heapLine)"
  sleep 10
done
wait "$DRIVER" || true
wait "$PID" || true
grep -E '^\s*\[mem:' "$TMP/run.log" || { echo "no RAE_MEM_STATS output (did the app quit?):"; tail -20 "$TMP/run.log"; }
