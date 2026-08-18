#!/usr/bin/env bash
# Regenerate the low-level Rae WebGPU bindings from the wgpu-native headers.
# Output is deterministic and checked in; run this only when the headers change
# or the generator changes. See docs/webgpu-bindings.md.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RAE="$REPO_ROOT/compiler/bin/rae"
WGPU_INC="${WGPU_NATIVE_INCLUDE:-$HOME/.local/wgpu-native/include/webgpu}"

if [ ! -x "$RAE" ]; then echo "build the compiler first: (cd compiler && make)"; exit 1; fi
if [ ! -f "$WGPU_INC/webgpu.h" ]; then echo "webgpu.h not found under $WGPU_INC (set WGPU_NATIVE_INCLUDE)"; exit 1; fi

"$RAE" bindgen \
  "$WGPU_INC/webgpu.h" \
  "$WGPU_INC/wgpu.h" \
  --out-dir "$REPO_ROOT/lib/webgpu" \
  --module webgpu \
  --import-prefix webgpu \
  --cheader "webgpu/wgpu.h" \
  --module-comment "WebGPU (webgpu.h + wgpu-native wgpu.h) low-level bindings."

echo "Regenerated lib/webgpu/{webgpu_enums,webgpu_types,webgpu}.rae"
