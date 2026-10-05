#!/usr/bin/env bash
# Build a Rae project to a standalone .wasm via the C backend + wasi-sdk.
#
#   compiler/tools/wasm_build.sh <project-dir> [entry.rae] [out.wasm]
#
# Pipeline: Rae --target compiled --emit-c  ->  wasi-sdk clang (wasm32-wasip1).
# wasi-sdk provides a matched clang + wasm-ld + wasi-sysroot + compiler-rt.
# Override its location with WASI_SDK (default: ~/.local/wasi-sdk).
set -euo pipefail

# Run from the rae root regardless of caller cwd (tools live in compiler/tools).
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1

WASI_SDK="${WASI_SDK:-$HOME/.local/wasi-sdk}"
RAE="${RAE:-compiler/bin/rae}"

PROJ="${1:?usage: wasm_build.sh <project-dir> [entry.rae] [out.wasm]}"
ENTRY="${2:-$PROJ/Main.rae}"
OUT="${3:-$PROJ/build/app.wasm}"

CC="$WASI_SDK/bin/clang"
SYS="$WASI_SDK/share/wasi-sysroot"
if [ ! -x "$CC" ]; then
  echo "wasm_build: wasi-sdk not found at $WASI_SDK" >&2
  echo "  install: download wasi-sdk for your platform and extract to \$WASI_SDK," >&2
  echo "  or set WASI_SDK=/path/to/wasi-sdk" >&2
  exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

"$RAE" build --target compiled --emit-c --project "$PROJ" --out "$TMP/out.c" "$ENTRY" >/dev/null

# Hand-written C glue in the project (e.g. fb_out.c) links alongside, minus the
# emitted runtime which the compiler drops next to out.c.
EXTRA_C="$(ls "$PROJ"/*.c 2>/dev/null | grep -v 'rae_runtime.c' || true)"

mkdir -p "$(dirname "$OUT")"
# Upstream Box3D's C library (lib/box3d, docs/physics-two-implementations.md
# §4): a program whose C includes its header links the wasm32 build
# tools/box3d/build.sh installed ($RAE_BOX3D, default ~/.cache/rae/box3d/install).
BOX3D_FLAGS=""
if grep -q '#include "box3d/box3d.h"' "$TMP/out.c"; then
  BOX3D="${RAE_BOX3D:-$HOME/.cache/rae/box3d/install}"
  if [ ! -f "$BOX3D/lib/wasm32/libbox3d.a" ]; then
    echo "wasm_build: this program imports box3d, but $BOX3D/lib/wasm32/libbox3d.a is missing; run tools/box3d/build.sh" >&2
    exit 2
  fi
  BOX3D_FLAGS="-I$BOX3D/include $BOX3D/lib/wasm32/libbox3d.a"
fi
# WASM_THREADS=1 builds a *threaded* module: wasm32-wasip1-threads + -pthread,
# so Rae `spawn` (which lowers to pthread_create) runs on real OS-thread-backed
# wasm threads — the same spawn code as the compiled target, no JS-side
# band-splitting hack. The module imports a *shared* memory so each worker the
# host spawns for `wasi.thread-spawn` instantiates over the SAME memory, and
# exports `wasi_thread_start`. -DRAE_WASM_THREADS keeps pthread_join in the
# runtime (it's a no-op stub on the single-threaded wasip1 target).
if [ "${WASM_THREADS:-0}" = "1" ]; then
  # Shared memory must be bounded: --max-memory=1 GiB (16384 * 64KiB pages).
  "$CC" --target=wasm32-wasip1-threads --sysroot="$SYS" -O2 -ffp-contract=off -msimd128 -pthread \
    -DRAE_WASM_THREADS \
    -Wl,--allow-undefined \
    -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=1073741824 \
    -o "$OUT" "$TMP/out.c" "$TMP/rae_runtime.c" $EXTRA_C $BOX3D_FLAGS -I"$TMP"
else
  # -msimd128: enable WASM SIMD (clang auto-vectorizes hot float loops); supported
  # by Node and all modern browsers. -Wl,--allow-undefined lets examples import
  # functions the host supplies from JS (e.g. fbPixel) as env imports.
  "$CC" --target=wasm32-wasip1 --sysroot="$SYS" -O2 -ffp-contract=off -msimd128 \
    -Wl,--allow-undefined \
    -o "$OUT" "$TMP/out.c" "$TMP/rae_runtime.c" $EXTRA_C $BOX3D_FLAGS -I"$TMP"
fi

echo "wasm_build: $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"
