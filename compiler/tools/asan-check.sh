#!/usr/bin/env bash
# asan-check.sh — run the memory-boundary fixtures under AddressSanitizer and
# UndefinedBehaviorSanitizer and fail on any report (docs/float4-design.md §9).
#
# The cases: fixtures whose subject is that nothing outside a buffer is read or
# written (the Float4 masked tail allocates every list with its exact length
# as capacity, so one element too far is a heap overflow). Each is the
# fixture's own program — the C the compiler emits plus the runtime — built
# with clang -fsanitize=address,undefined and run as `rae run` would, with the
# case's config.cmd flags that change the C (`--float4-scalar`). A case fails
# on a sanitizer report, a non-zero exit, or output that differs from its
# expected.txt. `make asan` runs it alone; a full suite run runs it as one
# pre-suite case. RAE_ASAN_FILTER="1085_float4_masked_tail" scopes the run;
# a compiler without the ASan runtime prints SKIP and passes.
set -u
cd "$(dirname "$0")/.." || exit 1     # -> compiler/
RAE="${RAE_BIN:-bin/rae}"
[ -x "$RAE" ] || { echo "error: $RAE not built (make -C compiler build)" >&2; exit 1; }
export RAE_FORMAT=check RAE_PROGRESS=off RAE_TOOLCHAIN_CHECK=off
CC="${RAE_ASAN_CC:-clang}"
TMP="$(mktemp -d -t rae_asan_XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

CASES="1085_float4_masked_tail 1086_float4_masked_tail_scalar"
if [ -n "${RAE_ASAN_FILTER:-}" ]; then CASES="$RAE_ASAN_FILTER"; fi

printf 'int main(void) { return 0; }\n' > "$TMP/probe.c"
if ! "$CC" -fsanitize=address,undefined "$TMP/probe.c" -o "$TMP/probe" >/dev/null 2>&1; then
  echo "SKIP: asan-check ($CC has no -fsanitize=address runtime)"
  exit 0
fi

FRAMEWORKS=""
if [ "$(uname)" = "Darwin" ]; then
  FRAMEWORKS="-framework Foundation -framework ImageIO -framework CoreGraphics"
fi

trim() { grep -v '^@@RAE_' "$1" | perl -0pe 's/\n+\z/\n/'; }
passed=0; failed=0
for name in $CASES; do
  dir="tests/cases/$name"
  work="$TMP/$name"
  mkdir -p "$work"
  defines=""
  if grep -q -- "--float4-scalar" "$dir/config.cmd" 2>/dev/null; then defines="-DRAE_FLOAT4_SCALAR"; fi
  if ! perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --target compiled --emit-c \
      --project "$dir" --out "$work/out.c" "$dir/Main.rae" > "$work/emit.log" 2>&1; then
    echo "FAIL: asan $name (emit failed)"; tail -5 "$work/emit.log" | sed 's/^/    /'
    failed=$((failed + 1)); continue
  fi
  # shellcheck disable=SC2086
  if ! perl -e 'alarm shift; exec @ARGV' 300 "$CC" -std=c11 -O1 -g -fno-omit-frame-pointer \
      -fsanitize=address,undefined -fno-sanitize-recover=all -ffp-contract=off -w $defines \
      "$work/out.c" "$work/rae_runtime.c" -I"$work" -I/opt/homebrew/include $FRAMEWORKS -lm -lpthread \
      -o "$work/app" > "$work/cc.log" 2>&1; then
    echo "FAIL: asan $name (ASan build failed)"; tail -5 "$work/cc.log" | sed 's/^/    /'
    failed=$((failed + 1)); continue
  fi
  (cd "$dir" && ASAN_OPTIONS="detect_leaks=0" perl -e 'alarm shift; exec @ARGV' 120 "$work/app") \
    > "$work/stdout" 2> "$work/stderr"
  rc=$?
  if grep -q "Sanitizer\|runtime error:" "$work/stderr"; then
    echo "FAIL: asan $name (sanitizer report)"
    grep -A12 "Sanitizer\|runtime error:" "$work/stderr" | head -30 | sed 's/^/    /'
    failed=$((failed + 1)); continue
  fi
  if [ "$rc" != "0" ]; then
    echo "FAIL: asan $name (exit $rc)"; tail -5 "$work/stderr" | sed 's/^/    /'
    failed=$((failed + 1)); continue
  fi
  cat "$work/stdout" "$work/stderr" > "$work/all"
  if [ -f "$dir/expected.txt" ] && ! diff -q <(trim "$work/stdout") <(trim "$dir/expected.txt" | grep -v '^warning: ') >/dev/null; then
    echo "FAIL: asan $name (output differs from expected.txt)"
    diff <(trim "$work/stdout") <(trim "$dir/expected.txt" | grep -v '^warning: ') | head -10 | sed 's/^/    /'
    failed=$((failed + 1)); continue
  fi
  passed=$((passed + 1))
done
if [ "$failed" -gt 0 ]; then
  echo "FAIL: asan-check ($failed of $((passed + failed)) programs failed)"
  exit 1
fi
echo "PASS: asan-check ($passed programs, no AddressSanitizer or UBSan report)"
