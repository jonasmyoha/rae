#!/bin/sh
# benchmarks/parallel_stages: a 220-stage solver-shaped step, as one
# parallelLoop per stage vs one flat parallelLoop, at 1, 2, 4 and 8 workers —
# and the same step with Box3D's own spin-barrier scheme in C, run alongside
# as the reference (on a loaded machine both barriers grow; compare them, not
# a fixed number).
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
  -I"$BUILD" -framework Foundation -framework ImageIO -framework CoreGraphics -o "$BUILD/parallel_stages"
t 120 cc -std=c11 -O2 -ffp-contract=off "$HERE/c/box3d_style.c" -o "$BUILD/box3d_style" -lpthread
# Side by side, so both see the same machine load: the Rae pool, then Box3D's
# own synchronisation scheme (c/box3d_style.c) on the same step.
for workers in 1 2 4 8; do
  RAE_WORKERS=$workers t 300 "$BUILD/parallel_stages" | grep '^RESULT,' | sed 's/^RESULT,/rae,/'
  t 120 "$BUILD/box3d_style" "$workers" 400 | grep '^RESULT,' | sed 's/^RESULT,/box3d,/'
done | awk -F, '{
  if ($1 == "rae" && $2 == 1) base = $3;
  printf "%-5s %d workers: staged %6.2f ms  flat %6.2f ms  barrier %5.2f us/stage", $1, $2, $3 / 1e6, $4 / 1e6, ($3 - $4) / 220 / 1000.0;
  if ($1 == "rae") printf "  staged speedup %.2fx", base / $3;
  printf "\n" }'
