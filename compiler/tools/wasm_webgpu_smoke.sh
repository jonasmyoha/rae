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
  examples/109_gpu3d_pbr/main.rae >/dev/null

test -s "$TMP/index.html"
test -s "$TMP/index.js"
test -s "$TMP/index.wasm"
grep -q 'id="canvas"' "$TMP/index.html"

echo "PASS wasm_webgpu_smoke: 109 browser bundle built"
