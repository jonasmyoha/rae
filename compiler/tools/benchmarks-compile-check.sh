#!/usr/bin/env bash
# benchmarks-compile-check.sh — every Rae benchmark program still compiles.
#
# The benchmarks (benchmarks/*/rae, benchmarks/servers/rae/*/*) are built only
# by their own run.sh, which nobody runs on every change; four of them stopped
# compiling under the frozen-`let` rule (0.1.200) and went unnoticed until a
# sweep. This emits each one's C (no C compile, no run) and fails on any
# error. One pre-suite case of every full run; standalone:
# `make benchmarks-check`.
set -u
cd "$(dirname "$0")/.." || exit 1     # -> compiler/
RAE="${RAE_BIN:-bin/rae}"
[ -x "$RAE" ] || { echo "error: $RAE not built (make -C compiler build)" >&2; exit 1; }
export RAE_FORMAT=check RAE_PROGRESS=off RAE_TOOLCHAIN_CHECK=off
TMP="$(mktemp -d -t rae_bench_check_XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

passed=0; failed=0
while IFS= read -r entry; do
  name=$(echo "$entry" | sed 's|^\.\./benchmarks/||; s|/Main.rae$||; s|/|_|g')
  mkdir -p "$TMP/$name"
  if perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --emit-c --out "$TMP/$name/out.c" --entry "$entry" \
      > "$TMP/$name.log" 2>&1; then
    passed=$((passed + 1))
  else
    echo "FAIL: benchmark ${entry#../} does not compile"
    grep -E ':[0-9]+:[0-9]+:' "$TMP/$name.log" | head -5 | sed 's/^/    /'
    failed=$((failed + 1))
  fi
done < <(find ../benchmarks -name Main.rae -path '*/rae/*' -not -path '*/build/*' -not -path '*/.rae/*' | sort)
if [ "$failed" -gt 0 ]; then
  echo "FAIL: benchmarks-compile-check ($failed of $((passed + failed)) benchmark programs do not compile)"
  exit 1
fi
echo "PASS: benchmarks-compile-check ($passed benchmark programs compile)"
