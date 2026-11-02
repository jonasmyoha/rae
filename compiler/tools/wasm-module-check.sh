#!/usr/bin/env bash
# wasm-module-check.sh — a WASM module build (docs/wasm-modules.md): the
# fixture tests/wasmModule (no main; its .raepack's wasm target exports `add`,
# `greet`, `summarize`, `doubled` and `flip`) is built with `rae build --target
# wasm` into a WASI reactor and called by name from tests/wasmModule/host.mjs
# under Node, whose output must equal expected.txt: only WASI imports, the
# exports, Int and String calls, an empty String result, 2000 calls in
# bounded memory; a struct, a List and an enum sent as their binary encoding
# and read back, the `rae.interface` section parsed with every hash checked,
# and a malformed value trapping with its export and parameter named. The same
# module then runs in a native Rae program through lib/wasm
# (tests/wasmEmbedHost and its expected.txt): built from the same frame
# types (the `frames` package, tests/wasmFrames), it checks the module's
# rae.interface, runs 1000 encoded frames in bounded memory, and refuses a
# module built from a changed AppFrameIn before calling it (skipped without
# tools/wamr/build.sh's library). Then three bad packs must be refused: an
# export that names no function, a `mod` parameter, and a struct holding a
# Ptr. SKIP without wasi-sdk ($WASI_SDK,
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

# a module built from a changed AppFrameIn (a field more): the native host's
# wasmCheckExport must refuse it before any call
mkdir -p "$TMP/changed"
cp -R "$FIXTURE" "$TMP/changed/wasmModule"
cp -R tests/wasmFrames "$TMP/changed/wasmFrames"
rm -f "$TMP/changed/wasmModule/rae.lock"
perl -0pi -e 's/  note: opt String\n/  note: opt String\n  urgent: Bool\n/' "$TMP/changed/wasmFrames/Frames.rae"
if ! grep -q "urgent: Bool" "$TMP/changed/wasmFrames/Frames.rae"; then
  echo "FAIL: wasm-module-check (could not change AppFrameIn for the refusal test)"; exit 1
fi
if ! perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --target wasm --project "$TMP/changed/wasmModule" \
    --out "$TMP/changed.wasm" "$TMP/changed/wasmModule/Main.rae" > "$TMP/changed.log" 2>&1; then
  echo "FAIL: wasm-module-check (the changed module did not build)"; tail -10 "$TMP/changed.log" | sed 's/^/    /'; exit 1
fi

# the same module in a native Rae host, through lib/wasm (WAMR)
WAMR_LIB="${RAE_WAMR:-$HOME/.cache/rae/wamr/install}/lib/librae_wamr.a"
EMBEDDED="the Rae host skipped (no $WAMR_LIB)"
if [ -f "$WAMR_LIB" ]; then
  if ! RAE_WASM_MODULE="$TMP/module.wasm" RAE_WASM_CHANGED_MODULE="$TMP/changed.wasm" perl -e 'alarm shift; exec @ARGV' 300 "$RAE" run tests/wasmEmbedHost/Main.rae \
      > "$TMP/host.raw" 2>&1; then
    echo "FAIL: wasm-module-check (the Rae host failed)"; tail -10 "$TMP/host.raw" | sed 's/^/    /'; exit 1
  fi
  grep -v '^@@RAE_' "$TMP/host.raw" > "$TMP/host.txt"
  if ! diff -q "$TMP/host.txt" tests/wasmEmbedHost/expected.txt > /dev/null; then
    echo "FAIL: wasm-module-check (the Rae host's output differs from tests/wasmEmbedHost/expected.txt)"
    diff "$TMP/host.txt" tests/wasmEmbedHost/expected.txt | head -12 | sed 's/^/    /'; exit 1
  fi
  EMBEDDED="run in a Rae host through lib/wasm (1000 typed frames)"
fi

# two packs a module build refuses
bad() {
  local name="$1" exports="$2" source="$3" expect="$4"
  mkdir -p "$TMP/$name"
  printf '%s\n' "$source" > "$TMP/$name/Main.rae"
  # the fixture's pack with these exports and without its `frames` dependency
  sed "s/exports: \"add, greet, summarize, doubled, flip\"/exports: \"$exports\"/" "$FIXTURE/Module.raepack" \
    | perl -0pe 's/  dependencies: \{.*?\n  \}\n//s' > "$TMP/$name/Module.raepack"
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
bad modParameter "total" 'func total(values: mod List(Int)) ret Int {
  ret values.length
}' "is not an Int, Float, Float64"
bad pointerParameter "keep" 'type Native {
  handle: Ptr
}

func keep(native: view Native) ret Int {
  ret 1
}' "cannot cross"

echo "PASS: wasm-module-check (a reactor exporting scalars, Strings and encoded values, called from Node with its rae.interface checked, $EMBEDDED; a changed frame type and bad exports refused)"
