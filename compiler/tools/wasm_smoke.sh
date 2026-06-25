#!/usr/bin/env bash
# W1 gate: build the raytracer-wasm spike to .wasm and run it under a Node WASI
# shim, asserting the framebuffer comes out the right size and looks like a lit
# scene (not all-black/all-white). This guards the Rae->C->WASM path so it
# can't silently rot.
#
# Skips cleanly (exit 0) when the wasm toolchain isn't present, so it never
# blocks environments without it. Run from the compiler/ directory.
set -uo pipefail

# Run from the rae root regardless of caller cwd (tools live in compiler/tools).
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1

WASI_SDK="${WASI_SDK:-$HOME/.local/wasi-sdk}"
PROJ="${1:-examples/46_raytracer_wasm_web}"
EXPECT_BYTES="${EXPECT_BYTES:-388800}"   # 480*270*3

command -v node >/dev/null 2>&1 || { echo "SKIP wasm_smoke: node not found"; exit 0; }
[ -x "$WASI_SDK/bin/clang" ] || { echo "SKIP wasm_smoke: wasi-sdk not at $WASI_SDK (set WASI_SDK)"; exit 0; }

OUT="$PROJ/build/app.wasm"
if ! compiler/tools/wasm_build.sh "$PROJ" "$PROJ/main.rae" "$OUT" >/dev/null 2>"/tmp/wasm_smoke_build.log"; then
  echo "FAIL wasm_smoke: wasm build"; cat /tmp/wasm_smoke_build.log; exit 1
fi

read -r BYTES AVG < <(node compiler/tools/wasm_run.mjs "$OUT" 2>/dev/null)
if [ "${BYTES:-0}" != "$EXPECT_BYTES" ]; then
  echo "FAIL wasm_smoke: got $BYTES bytes, expected $EXPECT_BYTES"; exit 1
fi
# avg byte must land in a sane lit-scene range (empirically ~163); guard against
# all-black (~0) / all-white (~255) / NaN regressions.
ok=$(node -e "const a=parseFloat(process.argv[1]); process.stdout.write(a>80&&a<210?'1':'0')" "$AVG")
if [ "$ok" != "1" ]; then
  echo "FAIL wasm_smoke: avg luminance $AVG out of range (80,210)"; exit 1
fi
echo "PASS wasm_smoke: $OUT — $BYTES bytes, avg $AVG"
