#!/bin/sh
# benchmarks/physics: Box3D's benchmark scenes (shared/benchmarks.c at 9f998c8)
# stepped single-threaded by the C library and by the Rae port
# (lib/physics, docs/physics-performance-plan.md §3):
#
#   c_scalar   Box3D's benchmark app built with BOX3D_DISABLE_SIMD
#   c_simd     the same with NEON / SSE2
#   rae        benchmarks/physics/rae, the release profile (lib/Float4's SIMD
#              lowering)
#
# Each scene runs the C app's step count (large_pyramid 200, many_pyramids
# 100, joint_grid 100, convex_pile 500; 60 Hz, 4 sub-steps, one worker) and
# prints the best of RUNS (default 3) in ms per step, the Rae position
# checksum and the ratios. The Box3D builds come from the oracle cache
# (docs/physics-performance-plan.md §9): no Box3D source is in this
# repository. RAE_BENCH_SCENES picks scenes; RAE_BENCH_CFLAGS adds flags to
# the Rae build.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAE_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
BUILD="$HERE/build"
CACHE="${RAE_BOX3D_CACHE:-$HOME/.cache/rae/box3d-oracle}"
RUNS="${RUNS:-3}"
SCENES="${RAE_BENCH_SCENES:-large_pyramid many_pyramids joint_grid convex_pile}"
mkdir -p "$BUILD"
t() { seconds=$1; shift; perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"; }

LOAD=$(uptime | sed 's/.*load averages*:[ ]*//' | awk '{print $1}')
CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
if [ "${BENCH_ALLOW_LOAD:-0}" != "1" ] && awk "BEGIN{exit !($LOAD > $CORES * 0.3)}"; then
  echo "refusing to benchmark: load average $LOAD on $CORES cores (BENCH_ALLOW_LOAD=1 overrides)." >&2
  exit 1
fi
for kind in scalar simd; do
  if [ ! -x "$CACHE/bench-$kind/bin/benchmark" ]; then
    echo "missing $CACHE/bench-$kind/bin/benchmark: build Box3D as docs/physics-performance-plan.md §9 describes" >&2
    exit 1
  fi
done

# The Rae program: one generated C file, compiled as the release profile does
RAE="$RAE_ROOT/compiler/bin/rae"
RAE_CFLAGS="-std=gnu11 -O2 -DNDEBUG -ffp-contract=off -mllvm -inline-threshold=400 ${RAE_BENCH_CFLAGS:-}"
LINK="-framework Foundation -framework ImageIO -framework CoreGraphics"
(cd "$RAE_ROOT/compiler" && t 300 "$RAE" build --target compiled --profile release --emit-c \
  --out "$BUILD/rae.c" "$HERE/rae/Main.rae" >/dev/null)
t 300 cc $RAE_CFLAGS "$BUILD/rae.c" "$BUILD/rae_runtime.c" -I"$BUILD" $LINK -o "$BUILD/rae" 2>/dev/null

# Best of RUNS, in ns per step. Box3D's app prints "run N : <ms> (ms)" for
# the whole scene.
c_best() {
  exe=$1; scene=$2
  # From the build directory: the app writes <scene>.csv into its cwd
  (cd "$BUILD" && t 900 "$exe" -t=1 -w=1 -r="$RUNS" -b="$scene") | awk -v scene="$scene" '
    /^benchmark:/ { split($0, parts, "steps = "); steps = parts[2] + 0 }
    /^run [0-9]+ :/ { ms = $4 + 0; if (best == "" || ms < best) best = ms }
    END { printf "%.0f\n", best * 1e6 / steps }'
}
rae_best() {
  scene=$1; best=""; sum=""
  for _ in $(seq 1 "$RUNS"); do
    line=$(t 900 "$BUILD/rae" "$scene" | grep '^RESULT,')
    ns=$(echo "$line" | cut -d, -f3)
    sum=$(echo "$line" | cut -d, -f4)
    if [ -z "$best" ] || [ "$ns" -lt "$best" ]; then best=$ns; fi
  done
  echo "$best $sum"
}

printf "%-14s %10s %10s %10s %8s %8s  %s\n" scene c_scalar c_simd rae rae/scal rae/simd checksum
for scene in $SCENES; do
  scalar=$(c_best "$CACHE/bench-scalar/bin/benchmark" "$scene")
  simd=$(c_best "$CACHE/bench-simd/bin/benchmark" "$scene")
  set -- $(rae_best "$scene")
  echo "$scene $scalar $simd $1 $2" | awk '{
    printf "%-14s %8.2fms %8.2fms %8.2fms %7.2fx %7.2fx  %s\n", $1, $2 / 1e6, $3 / 1e6, $4 / 1e6, $4 / $2, $4 / $3, $5
  }'
done
