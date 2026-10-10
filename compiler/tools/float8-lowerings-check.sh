#!/usr/bin/env bash
# float8-lowerings-check.sh — lib/Float8's other lowerings (docs/float4-design.md §10).
#
# Fixture 1088_float8_bit_exact checks every Float8 function bit for bit on the
# machine the suite runs on. This builds the same program for x86-64 twice: with
# AVX2 (one 256-bit register per Float8) and without (two SSE2 registers).
#   - on an x86-64 machine both builds RUN and must print 1088's expected
#     output (the lowering line aside); the AVX2 one only where the CPU has it;
#   - on Apple Silicon they are cross-compiled (clang -arch x86_64): each must
#     compile without warnings and the AVX2 one must use ymm registers. They run
#     only where x86-64 binaries can (Rosetta); otherwise the check says so.
# The wasm SIMD128 build (two v128 registers per Float8) is built with emcc and
# run under Node, and must print the same output (SKIP without emcc / node).
# One pre-suite case of every full run; `make float8-lowerings` alone.
set -u
cd "$(dirname "$0")/.." || exit 1     # -> compiler/
RAE="${RAE_BIN:-bin/rae}"
[ -x "$RAE" ] || { echo "error: $RAE not built (make -C compiler build)" >&2; exit 1; }
export RAE_FORMAT=check RAE_PROGRESS=off RAE_TOOLCHAIN_CHECK=off
CASE=tests/cases/1088_float8_bit_exact
TMP="$(mktemp -d -t rae_float8_x86_XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

ARCH_FLAGS=""
case "$(uname -m)" in
  x86_64|amd64) ;;
  arm64)
    if [ "$(uname)" = "Darwin" ]; then ARCH_FLAGS="-arch x86_64"; else
      echo "SKIP: float8-lowerings-check (no x86-64 cross compiler on $(uname -s) $(uname -m))"; exit 0; fi ;;
  *) echo "SKIP: float8-lowerings-check (no x86-64 toolchain for $(uname -m))"; exit 0 ;;
esac
FRAMEWORKS=""
[ "$(uname)" = "Darwin" ] && FRAMEWORKS="-framework Foundation -framework ImageIO -framework CoreGraphics"

if ! perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --emit-c --out "$TMP/emit/out.c" --entry "$CASE/Main.rae" \
    > "$TMP/emit.log" 2>&1; then
  echo "FAIL: float8-lowerings-check (emit failed)"; tail -5 "$TMP/emit.log" | sed 's/^/    /'; exit 1
fi
# Can this machine run an x86-64 binary at all?
printf 'int main(void) { return 0; }\n' > "$TMP/probe.c"
can_run=0
if clang $ARCH_FLAGS "$TMP/probe.c" -o "$TMP/probe" >/dev/null 2>&1 && "$TMP/probe" >/dev/null 2>&1; then can_run=1; fi

trim() { grep -v '^@@RAE_' "$1" | grep -v '^warning: ' | grep -v '^lowering is scalar' | perl -0pe 's/\n+\z/\n/'; }
failed=0; notes=""
for variant in sse2 avx2; do
  extra=""
  [ "$variant" = avx2 ] && extra="-mavx2"
  # shellcheck disable=SC2086
  if ! perl -e 'alarm shift; exec @ARGV' 300 clang $ARCH_FLAGS -std=c11 -O2 -ffp-contract=off $extra \
      -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
      -Wno-unused-but-set-variable -Wno-sign-compare -Wno-missing-field-initializers -Wno-unused-label \
      -Wno-unused-value -Wno-incompatible-pointer-types-discards-qualifiers \
      "$TMP/emit/out.c" "$TMP/emit/rae_runtime.c" -I"$TMP/emit" -I/opt/homebrew/include $FRAMEWORKS -lm \
      -o "$TMP/app-$variant" > "$TMP/cc-$variant.log" 2>&1; then
    echo "FAIL: float8-lowerings-check ($variant does not compile)"; grep -E "error|warning" "$TMP/cc-$variant.log" | head -5 | sed 's/^/    /'
    failed=1; continue
  fi
  if [ "$variant" = avx2 ] && command -v objdump >/dev/null 2>&1; then
    if ! objdump -d "$TMP/app-avx2" 2>/dev/null | grep -q ymm; then
      echo "FAIL: float8-lowerings-check (the AVX2 build uses no ymm register)"; failed=1; continue
    fi
  fi
  if [ "$can_run" = 0 ]; then notes="$notes $variant:compiled"; continue; fi
  if [ "$variant" = avx2 ] && ! grep -q avx2 /proc/cpuinfo 2>/dev/null \
      && ! sysctl -n machdep.cpu.leaf7_features 2>/dev/null | grep -qi avx2; then
    notes="$notes avx2:compiled(cpu-has-none)"; continue
  fi
  (cd "$CASE" && perl -e 'alarm shift; exec @ARGV' 120 "$TMP/app-$variant") > "$TMP/out-$variant" 2>&1
  if ! diff -q <(trim "$TMP/out-$variant") <(trim "$CASE/expected.txt") >/dev/null; then
    echo "FAIL: float8-lowerings-check ($variant output differs from 1088's expected)"
    diff <(trim "$TMP/out-$variant") <(trim "$CASE/expected.txt") | head -10 | sed 's/^/    /'
    failed=1; continue
  fi
  notes="$notes $variant:bit-exact"
done
# wasm SIMD128, run under Node
if command -v emcc >/dev/null 2>&1 && command -v node >/dev/null 2>&1; then
  if perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --target wasm --profile release --project "$CASE" \
      --out "$TMP/wasm/app.mjs" "$CASE/Main.rae" > "$TMP/wasm.log" 2>&1; then
    printf '%s\n' "import createRaeApp from './app.mjs';" "const lines = [];" \
      "await createRaeApp({ print: (t) => lines.push(t), printErr: (t) => lines.push(t) });" \
      "console.log(lines.join('\\n'));" > "$TMP/wasm/run.mjs"
    (cd "$TMP/wasm" && perl -e 'alarm shift; exec @ARGV' 120 node run.mjs) > "$TMP/out-wasm" 2>&1
    if diff -q <(trim "$TMP/out-wasm") <(trim "$CASE/expected.txt") >/dev/null; then
      notes="$notes wasm:bit-exact"
    else
      echo "FAIL: float8-lowerings-check (wasm output differs from 1088's expected)"
      diff <(trim "$TMP/out-wasm") <(trim "$CASE/expected.txt") | head -10 | sed 's/^/    /'
      failed=1
    fi
  else
    echo "FAIL: float8-lowerings-check (wasm build failed)"; tail -5 "$TMP/wasm.log" | sed 's/^/    /'; failed=1
  fi
else
  notes="$notes wasm:skipped(no-emcc-or-node)"
fi
[ "$failed" = 0 ] || exit 1
if [ "$can_run" = 0 ]; then
  echo "PASS: float8-lowerings-check ($notes; x86-64 not run: this machine cannot run x86-64)"
else
  echo "PASS: float8-lowerings-check ($notes )"
fi
