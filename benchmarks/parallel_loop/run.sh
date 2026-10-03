#!/bin/sh
# benchmarks/parallel_loop: parallelLoop launch cost and compute speedup at
# 1, 2, 4 and 8 workers (RAE_WORKERS). Release build, -O2 -ffp-contract=off.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAE_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
BUILD="$HERE/build"
mkdir -p "$BUILD"
t() { seconds=$1; shift; perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"; }
echo "load: $(uptime | sed 's/.*load averages*:[ ]*//')"
t 300 "$RAE_ROOT/compiler/bin/rae" build --target compiled --profile release --emit-c \
  --out "$BUILD/rae_generated.c" "$HERE/rae/Main.rae" >/dev/null
t 300 cc -std=c11 -O2 -DNDEBUG -ffp-contract=off "$BUILD/rae_generated.c" "$BUILD/rae_runtime.c" \
  -I"$BUILD" -framework Foundation -framework ImageIO -framework CoreGraphics -o "$BUILD/parallel_loop"
for workers in 1 2 4 8; do
  RAE_WORKERS=$workers t 300 "$BUILD/parallel_loop" | grep '^RESULT,'
done | awk -F, '
  $2 == "launch"  { printf "launch   %d workers: %6.2f us per parallelLoop (back to back)\n", $3, $4 / 1000.0 }
  $2 == "cold"    { printf "cold     %d workers: %6.2f us per parallelLoop (after 2 ms idle)\n", $3, $4 / 1000.0 }
  $2 == "compute" { if ($3 == 1) base = $4; sum[$3] = $5;
                    printf "compute  %d workers: %7.2f ms  speedup %.2fx  checksum %s\n", $3, $4 / 1e6, base / $4, $5 }'
