#!/bin/bash
# The compiler's speed on one standard program: the front end and C emission
# (parse, sema, codegen; no C compiler) of a featured example, in Rae source
# lines per second. Best of two emits, since the machine is often busy. Prints
#
#   @@RAE_COMPILER_SPEED@@ example=<id> lines=<n> emit_ms=<n> lines_per_s=<n>
#
# which the devtools test-log tailer records as `compiler.lines_per_s` (the
# Statistics tab's "Compiler speed" graph). The standard program is
# 119_ocean_fft: featured, mid-sized (~39 000 lines with the stdlib), and on
# the graph's history since 2026-09-17. The emit is superlinear in program
# size, so the number is only comparable for the SAME program; change it only
# with a note on the graph. Never fails the suite: it measures.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
EXAMPLE="${RAE_SPEED_EXAMPLE:-119_ocean_fft}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
best_ms=""; lines=""
for _ in 1 2; do
  line=$(RAE_BUILD_TIME=1 RAE_FORMAT=check perl -e 'alarm shift; exec @ARGV' 300 \
    compiler/bin/rae build --emit-c --out "$OUT/speed.c" "examples/$EXAMPLE/Main.rae" 2>&1 \
    | grep -m1 '^@@RAE_BUILD_TIME@@')
  ms=$(printf '%s' "$line" | sed -n 's/.* emit_ms=\([0-9]*\).*/\1/p')
  n=$(printf '%s' "$line" | sed -n 's/.* lines=\([0-9]*\).*/\1/p')
  [ -z "$ms" ] || [ -z "$n" ] && continue
  lines=$n
  if [ -z "$best_ms" ] || [ "$ms" -lt "$best_ms" ]; then best_ms=$ms; fi
done
if [ -z "$best_ms" ] || [ "$best_ms" -le 0 ]; then
  echo "compiler-speed: SKIP ($EXAMPLE did not build)"
  exit 0
fi
speed=$((lines * 1000 / best_ms))
echo "compiler-speed: $EXAMPLE, $lines lines in $best_ms ms of emit, $speed lines/s"
echo "@@RAE_COMPILER_SPEED@@ example=$EXAMPLE lines=$lines emit_ms=$best_ms lines_per_s=$speed"
