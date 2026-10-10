#!/usr/bin/env bash
# Compile the raster 3D example through the public browser-WASM build target.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."

command -v emcc >/dev/null 2>&1 || { echo "SKIP wasm_webgpu_smoke: emcc not found"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

perl -e 'alarm shift; exec @ARGV' 240 compiler/bin/rae build \
  --target wasm --profile dev \
  --project examples/109_gpu3d_pbr \
  --out "$TMP/index.html" \
  examples/109_gpu3d_pbr/Main.rae >/dev/null

test -s "$TMP/index.html"
test -s "$TMP/index.js"
test -s "$TMP/index.wasm"
grep -q 'id="canvas"' "$TMP/index.html"

perl -e 'alarm shift; exec @ARGV' 240 compiler/bin/rae build \
  --target wasm --profile dev \
  --project examples/112_metaballs_deferred \
  --out "$TMP/ui.html" \
  examples/112_metaballs_deferred/Main.rae >/dev/null

test -s "$TMP/ui.html"
test -s "$TMP/ui.js"
test -s "$TMP/ui.wasm"
test -s "$TMP/ui.data"
grep -q 'id="canvas"' "$TMP/ui.html"

perl -e 'alarm shift; exec @ARGV' 240 compiler/bin/rae build \
  --target wasm --profile dev \
  --project examples/109_gpu3d_pbr \
  --out "$TMP/app.mjs" \
  examples/109_gpu3d_pbr/Main.rae >/dev/null

test -s "$TMP/app.mjs"
test -s "$TMP/app.wasm"
grep -q 'createRaeApp' "$TMP/app.mjs"

# The C-library physics track links upstream Box3D's Emscripten build
# (tools/box3d/build.sh installs it when emcc is present;
# docs/physics-two-implementations.md §6). Built when that library exists.
box3d_note=""
BOX3D_WEB_LIB="${RAE_BOX3D:-$HOME/.cache/rae/box3d/install}/lib/emscripten/libbox3d.a"
if [ -f "$BOX3D_WEB_LIB" ]; then
  perl -e 'alarm shift; exec @ARGV' 300 compiler/bin/rae build \
    --target wasm --profile dev \
    --project examples/125_vehicle_c \
    --out "$TMP/box3d/index.html" \
    examples/125_vehicle_c/Main.rae >/dev/null
  test -s "$TMP/box3d/index.wasm"
  box3d_note="; the Box3D C track's browser build"
fi

# A nonblocking GPU readback completes in the browser and copies the right
# bytes (examples/zz_web_readback_check). It once never produced a sample:
# the runtime's allocation-size check answered 0 under Emscripten, so every
# copy out of a mapped buffer was refused without a word. Built always; run in
# headless Chrome when Chrome is installed (skipped otherwise, and when the
# browser offers no WebGPU).
perl -e 'alarm shift; exec @ARGV' 240 compiler/bin/rae build \
  --target wasm --profile dev \
  --project examples/zz_web_readback_check \
  --out "$TMP/readback/index.html" \
  examples/zz_web_readback_check/Main.rae >/dev/null
test -s "$TMP/readback/index.wasm"

# A spawned task that sleeps runs between the frames of a mainLoop: it
# suspends at each sleep and the frames carry it on
# (docs/lightweight-spawn-design.md §16). Built always; run in headless
# Chrome when Chrome is installed.
perl -e 'alarm shift; exec @ARGV' 240 compiler/bin/rae build \
  --target wasm --profile dev \
  --project examples/zz_web_task_check \
  --out "$TMP/task/index.html" \
  examples/zz_web_task_check/Main.rae >/dev/null
test -s "$TMP/task/index.wasm"

CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
if [ ! -x "$CHROME" ]; then CHROME="$(command -v google-chrome || command -v chromium || true)"; fi
have_chrome=0
if [ -n "$CHROME" ] && [ -x "$CHROME" ] && command -v python3 >/dev/null 2>&1; then have_chrome=1; fi

# The console lines of one page served from directory $1, in headless Chrome
page_console() {
  local port=$((20000 + ($$ + $2) % 20000))
  (cd "$1" && exec perl -e 'alarm shift; exec @ARGV' 90 python3 -m http.server "$port" --bind 127.0.0.1) >/dev/null 2>&1 &
  local server=$!
  sleep 1
  perl -e 'alarm shift; exec @ARGV' 60 "$CHROME" --headless=new --enable-unsafe-webgpu \
    --enable-logging=stderr --v=0 --no-first-run --user-data-dir="$1/chrome" \
    "http://127.0.0.1:$port/index.html" 2>&1 | grep CONSOLE || true
  kill "$server" 2>/dev/null || true
  wait "$server" 2>/dev/null || true
}

task_note="task page built (no Chrome: not run)"
if [ "$have_chrome" = 1 ]; then
  console="$(page_console "$TMP/task" 1)"
  if printf '%s' "$console" | grep -q 'TASK OK'; then
    task_note="a waiting task ran between frames in headless Chrome"
  else
    echo "FAIL wasm_webgpu_smoke: the spawned task did not run between frames:"
    printf '%s\n' "$console" | sed 's/.*CONSOLE:[0-9]*\] "//;s/", source.*//' | head -10
    exit 1
  fi
fi

readback_note="readback page built (no Chrome: not run)"
if [ "$have_chrome" = 1 ]; then
  console="$(page_console "$TMP/readback" 2)"
  if printf '%s' "$console" | grep -q 'READBACK OK'; then
    readback_note="readback completed in headless Chrome"
  elif printf '%s' "$console" | grep -q 'READBACK FAIL: no GPU'; then
    readback_note="readback page built (Chrome has no WebGPU here: not run)"
  else
    echo "FAIL wasm_webgpu_smoke: the browser readback did not complete:"
    printf '%s\n' "$console" | sed 's/.*CONSOLE:[0-9]*\] "//;s/", source.*//' | head -10
    exit 1
  fi
fi

echo "PASS wasm_webgpu_smoke: 109 and composed 110 browser pages plus embeddable module built$box3d_note; $readback_note; $task_note"
