#!/usr/bin/env bash
# wasm-module-check.sh — a WASM module build (docs/wasm-modules.md): the
# fixture tests/wasmModule (no main; its .raepack's wasm target exports `add`
# and `greet`) is built with `rae build --target wasm` into a WASI reactor
# and called by name from tests/wasmModule/host.mjs under Node, whose output
# must equal expected.txt: only WASI imports, the exports, Int and String
# calls, an empty String result, and 2000 calls in bounded memory. The same
# module then runs in a native Rae program through lib/wasm
# (tests/wasmEmbedHost and its expected.txt; skipped without
# tools/wamr/build.sh's library). Then two bad packs must be refused: an
# export that names no function, and one whose parameter type a module
# cannot export. SKIP without wasi-sdk ($WASI_SDK,
# default ~/.local/wasi-sdk) or node. One pre-suite case of every full run;
# `make wasm-module-check` alone.
set -u
cd "$(dirname "$0")/.." || exit 1     # -> compiler/
RAE="${RAE_BIN:-bin/rae}"
[ -x "$RAE" ] || { echo "error: $RAE not built (make -C compiler build)" >&2; exit 1; }
export RAE_FORMAT=check RAE_PROGRESS=off RAE_TOOLCHAIN_CHECK=off
WASI_SDK="${WASI_SDK:-$HOME/.local/wasi-sdk}"
if [ ! -x "$WASI_SDK/bin/clang" ]; then echo "SKIP: wasm-module-check (no wasi-sdk at $WASI_SDK)"; exit 0; fi
if ! command -v node >/dev/null 2>&1; then echo "SKIP: wasm-module-check (no node)"; exit 0; fi
FIXTURE=tests/wasmModule
TMP="$(mktemp -d -t rae_wasm_module_XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

if ! perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --target wasm --project "$FIXTURE" --out "$TMP/module.wasm" \
    "$FIXTURE/Main.rae" > "$TMP/build.log" 2>&1; then
  echo "FAIL: wasm-module-check (the module did not build)"; tail -10 "$TMP/build.log" | sed 's/^/    /'; exit 1
fi
if ! perl -e 'alarm shift; exec @ARGV' 60 node --no-warnings "$FIXTURE/host.mjs" "$TMP/module.wasm" > "$TMP/out.txt" 2>&1; then
  echo "FAIL: wasm-module-check (the host failed)"; tail -10 "$TMP/out.txt" | sed 's/^/    /'; exit 1
fi
if ! diff -q "$TMP/out.txt" "$FIXTURE/expected.txt" > /dev/null; then
  echo "FAIL: wasm-module-check (the host's output differs from expected.txt)"
  diff "$TMP/out.txt" "$FIXTURE/expected.txt" | head -12 | sed 's/^/    /'; exit 1
fi

# the same module in a native Rae host, through lib/wasm (WAMR)
WAMR_LIB="${RAE_WAMR:-$HOME/.cache/rae/wamr/install}/lib/librae_wamr.a"
EMBEDDED="the Rae host skipped (no $WAMR_LIB)"
if [ -f "$WAMR_LIB" ]; then
  if ! RAE_WASM_MODULE="$TMP/module.wasm" perl -e 'alarm shift; exec @ARGV' 300 "$RAE" run tests/wasmEmbedHost/Main.rae \
      > "$TMP/host.raw" 2>&1; then
    echo "FAIL: wasm-module-check (the Rae host failed)"; tail -10 "$TMP/host.raw" | sed 's/^/    /'; exit 1
  fi
  grep -v '^@@RAE_' "$TMP/host.raw" > "$TMP/host.txt"
  if ! diff -q "$TMP/host.txt" tests/wasmEmbedHost/expected.txt > /dev/null; then
    echo "FAIL: wasm-module-check (the Rae host's output differs from tests/wasmEmbedHost/expected.txt)"
    diff "$TMP/host.txt" tests/wasmEmbedHost/expected.txt | head -12 | sed 's/^/    /'; exit 1
  fi
  EMBEDDED="run in a Rae host through lib/wasm"
fi

# two packs a module build refuses
bad() {
  local name="$1" exports="$2" source="$3" expect="$4"
  mkdir -p "$TMP/$name"
  printf '%s\n' "$source" > "$TMP/$name/Main.rae"
  sed "s/exports: \"add, greet\"/exports: \"$exports\"/" "$FIXTURE/Module.raepack" > "$TMP/$name/Module.raepack"
  if RAE_FORMAT=off perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --target wasm --project "$TMP/$name" \
      --out "$TMP/$name/m.wasm" "$TMP/$name/Main.rae" > "$TMP/$name/build.log" 2>&1; then
    echo "FAIL: wasm-module-check (the $name pack built)"; exit 1
  fi
  if ! grep -q "$expect" "$TMP/$name/build.log"; then
    echo "FAIL: wasm-module-check (the $name pack failed without saying: $expect)"
    tail -5 "$TMP/$name/build.log" | sed 's/^/    /'; exit 1
  fi
}
bad missing "nothing" 'func add(a: view Int, b: view Int) ret Int {
  ret a + b
}' "is not a function of the program"
bad listParameter "total" 'func total(values: view List(Int)) ret Int {
  ret values.length
}' "is not an Int, Float, Float64"

echo "PASS: wasm-module-check (a reactor exporting add and greet, called from Node, $EMBEDDED; bad exports refused)"
