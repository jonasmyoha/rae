#!/bin/bash
# c-warnings-gate.sh: one pre-suite case. The C the compiler generates must be
# warning-free: `rae run` compiles it with `-w`, which hid 564 const-dropping
# and pointer-type warnings in 121_ui_editor until they were fixed, and the
# browser (emcc) build showed them all. This emits the C of the largest apps
# and of the browser-smoke apps and compiles it, without linking, with
# `-Wall -Wextra -Werror` (the unused-*, sign-compare and
# missing-field-initializers categories off: generated code declares what a
# template might use). Any warning fails the case. About a minute; the apps
# are emitted in parallel.
set -u
cd "$(dirname "$0")/.." || exit 1
BIN="${RAE_BIN:-$PWD/bin/rae}"
ROOT="$(cd .. && pwd)"
CC="${CC_FOR_WARNINGS:-clang}"
if ! command -v "$CC" >/dev/null 2>&1; then
  echo "SKIP: c-warnings-gate (no $CC)"
  exit 0
fi
WG="${WGPU_NATIVE:-$HOME/.local/wgpu-native}"
B3="$HOME/.cache/rae/box3d/install/include"
APPS="121_ui_editor 106_mobile_ui 122_physics_playground_port 109_gpu3d_pbr 112_metaballs_deferred 125_vehicle_c"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for app in $APPS; do
  (
    mkdir -p "$WORK/$app"
    cd "$ROOT" || exit 1
    perl -e 'alarm shift; exec @ARGV' 400 "$BIN" build --emit-c --entry "examples/$app/Main.rae" \
      --out "$WORK/$app/out.c" > "$WORK/$app/emit.log" 2>&1
    echo $? > "$WORK/$app/emit.rc"
  ) &
done
wait

failed=""
total=0
for app in $APPS; do
  if [ "$(cat "$WORK/$app/emit.rc" 2>/dev/null)" != "0" ] || [ ! -f "$WORK/$app/out.c" ]; then
    failed="$failed $app(emit)"
    continue
  fi
  "$CC" -fsyntax-only -std=c11 -Wall -Wextra -Werror \
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable \
    -Wno-missing-field-initializers -Wno-sign-compare \
    -DRAE_HAS_SDL3 -DRAE_HAS_WEBGPU -I"$WORK/$app" -I/opt/homebrew/include -I"$WG/include" -I"$B3" \
    "$WORK/$app/out.c" > "$WORK/$app/cc.log" 2>&1
  count=$(grep -c ' error: ' "$WORK/$app/cc.log")
  total=$((total + count))
  if [ "$count" != "0" ]; then
    failed="$failed $app"
    echo "  $app: $count warning(s)"
    grep -A1 ' error: ' "$WORK/$app/cc.log" | head -12 | sed 's/^/    /'
  fi
done
if [ -n "$failed" ]; then
  echo "FAIL: c-warnings-gate (generated C with warnings:$failed)"
  exit 1
fi
echo "PASS: c-warnings-gate (the generated C of $(echo $APPS | wc -w | tr -d ' ') apps compiles with -Wall -Wextra -Werror)"
