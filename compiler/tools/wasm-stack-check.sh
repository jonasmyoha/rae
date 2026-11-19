#!/usr/bin/env bash
# wasm-stack-check.sh — a WASM build has a stack of a native build's order (docs/wasm-modules.md,
# "The stack"). Builds compiler/tests/wasmStack (2,000 calls whose frames keep a 16-number record
# in the linear memory's stack, about 400 KB) with compiler/tools/wasm_build.sh and runs it under
# Node: it must end and print its sum. The same program linked with wasi-sdk's old 64 KB stack
# (RAE_WASM_STACK_BYTES=65536) must stop with "memory access out of bounds", so the check really
# goes past 64 KB. SKIPs without wasi-sdk or node. One pre-suite case of every full run.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
WASI_SDK="${WASI_SDK:-$HOME/.local/wasi-sdk}"
command -v node >/dev/null 2>&1 || { echo "SKIP: wasm-stack-check (node not found)"; exit 0; }
[ -x "$WASI_SDK/bin/clang" ] || { echo "SKIP: wasm-stack-check (wasi-sdk not at $WASI_SDK)"; exit 0; }
PROJ=compiler/tests/wasmStack
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if ! compiler/tools/wasm_build.sh "$PROJ" "$PROJ/Main.rae" "$TMP/deep.wasm" > "$TMP/build.log" 2>&1; then
  echo "FAIL: wasm-stack-check (build)"; cat "$TMP/build.log"; exit 1
fi
out=$(perl -e 'alarm shift; exec @ARGV' 60 node compiler/tools/wasm_run.mjs --echo "$TMP/deep.wasm" 2>&1)
if ! printf '%s\n' "$out" | grep -q '^deep stack: 2031000$'; then
  echo "FAIL: wasm-stack-check (the recursion did not finish under the default stack)"
  printf '%s\n' "$out" | head -5 | sed 's/^/  /'
  exit 1
fi
if ! RAE_WASM_STACK_BYTES=65536 compiler/tools/wasm_build.sh "$PROJ" "$PROJ/Main.rae" "$TMP/small.wasm" > "$TMP/build-small.log" 2>&1; then
  echo "FAIL: wasm-stack-check (build with 64 KB)"; cat "$TMP/build-small.log"; exit 1
fi
small=$(perl -e 'alarm shift; exec @ARGV' 60 node compiler/tools/wasm_run.mjs --echo "$TMP/small.wasm" 2>&1)
if ! printf '%s\n' "$small" | grep -q 'memory access out of bounds'; then
  echo "FAIL: wasm-stack-check (with a 64 KB stack the recursion should run out of it; the check no longer goes past 64 KB)"
  printf '%s\n' "$small" | head -3 | sed 's/^/  /'
  exit 1
fi
echo "PASS: wasm-stack-check (a recursion of about 400 KB of stack runs; with 64 KB it runs out)"
