#!/usr/bin/env bash
# Canvas-hover memory test for the UI editor: builds the editor, runs it headless for
# SECONDS (default 600) with the pointer alternating between two canvas nodes
# every frame (RAE_GPU2D_TEST_POINTER, runtime_gpu2d_platform.c), so each frame
# re-hovers a node through the real loop — hit-test, hover outline, the
# hierarchy refresh, paint. Samples resident memory every 10 s, prints the
# runtime's outstanding String/buffer counts every 10 s WHILE it runs
# (RAE_MEM_STATS_EVERY_MS, the `[mem:live]` series) and again at exit
# (RAE_MEM_STATS=1). The live series is the one that shows memory piling up in
# a container that is still in use: the exit counts are taken after teardown.
#
#   bash examples/121_ui_editor/tools/hover-memory-test.sh [SECONDS] [sweep]
#
# With `sweep`, the pointer instead walks the canvas column x=700 from the top to
# the bottom of the window in 180 steps and starts again, so it hovers most of
# the UI in turn (the `sweep:` form of RAE_GPU2D_TEST_POINTER).
#
# hover-memory-test-window.sh is the same memory test in a visible window, driven by the real
# mouse (mouse-sweep.c), with `heap` snapshots added.
#
# Report only: it does not fail on growth. Note that `leaks` cannot be used on
# this run — RAE_MEM_STATS keeps a table of every live allocation, which makes
# leaked blocks look referenced; run `leaks` on a separate run without it.
set -euo pipefail
SECONDS_TO_RUN="${1:-600}"
POINTER="623,196,787,297,1"
[ "${2:-}" = "sweep" ] && POINTER="sweep:700,5,895,180"
REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$REPO_ROOT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
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
# Title and OrbitPlanet in MainMenu at the default 1400x900 headless canvas.
env RAE_UI_EDITOR_SCENE=examples/121_ui_editor/assets/samples/MainMenu.raescene \
  RAE_UI_EDITOR_ROOT=examples/121_ui_editor/assets/samples \
  RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=$((SECONDS_TO_RUN * 1000)) RAE_MEM_STATS=1 \
  RAE_MEM_STATS_EVERY_MS=10000 \
  RAE_GPU2D_TEST_POINTER="$POINTER" \
  perl -e 'alarm shift; exec @ARGV' $((SECONDS_TO_RUN + 60)) "$TMP/app" > "$TMP/run.log" 2>&1 &
PID=$!
echo "seconds rss_kb"
for t in $(seq 0 10 "$SECONDS_TO_RUN"); do
  RSS=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ') || true
  [ -z "$RSS" ] && break
  echo "$t $RSS"
  sleep 10
done
wait "$PID" || true
grep -a "test-pointer" "$TMP/run.log" | tail -1
echo "live outstanding (while running):"
grep -a "^\[mem:live\]" "$TMP/run.log"
echo "at exit (after teardown):"
grep -a "mem:string:sub \|mem:string:interp\|mem:string:TOTAL\|mem:buf" "$TMP/run.log"
