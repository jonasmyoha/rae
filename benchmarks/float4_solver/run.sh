#!/bin/sh
# benchmarks/float4_solver: the wide convex contact kernels (warm start, push,
# relax) four ways on the same synthetic constraints (docs/float4-design.md):
#
#   c_scalar     Box3D's contact_solver.c, BOX3D_DISABLE_SIMD
#   c_simd       Box3D's contact_solver.c, NEON / SSE2
#   rae_scalar   lib/physics/dynamics as it is, with lib/Float4 forced onto
#                its scalar definitions (-DRAE_FLOAT4_SCALAR)
#   rae_<simd>   the same program with lib/Float4's SIMD lowering
#                (rae_neon on arm64, rae_sse2 on x86-64)
#
# Every build must print the same checksum (bit-exact with the scalar path).
# Prints the best of five runs of each and the ratios. The Box3D builds come
# from the oracle cache (tools/box3d-oracle): no Box3D source is in this
# repository. Rae is built release (-O2 -DNDEBUG -ffp-contract=off -mllvm
# -inline-threshold=400), as rae's release profile compiles; RAE_BENCH_CFLAGS
# adds flags to the Rae builds (a later -inline-threshold wins, so
# RAE_BENCH_CFLAGS="-mllvm -inline-threshold=225" measures clang's default).
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAE_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
BUILD="$HERE/build"
CACHE="${RAE_BOX3D_CACHE:-$HOME/.cache/rae/box3d-oracle}"
SRC="$CACHE/box3d"
mkdir -p "$BUILD"
t() { seconds=$1; shift; perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"; }

LOAD=$(uptime | sed 's/.*load averages*:[ ]*//' | awk '{print $1}')
CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
if [ "${BENCH_ALLOW_LOAD:-0}" != "1" ] && awk "BEGIN{exit !($LOAD > $CORES * 0.3)}"; then
  echo "refusing to benchmark: load average $LOAD on $CORES cores (BENCH_ALLOW_LOAD=1 overrides)." >&2
  exit 1
fi
for kind in scalar simd; do
  if [ ! -f "$CACHE/bench-$kind/src/libbox3d.a" ] && [ -z "$(find "$CACHE/bench-$kind" -name libbox3d.a 2>/dev/null)" ]; then
    echo "missing $CACHE/bench-$kind: build Box3D as docs/physics-performance-plan.md §9 describes" >&2
    exit 1
  fi
done

# Box3D's kernels
for kind in scalar simd; do
  extra=""
  if [ "$kind" = scalar ]; then extra="-DBOX3D_DISABLE_SIMD"; fi
  lib=$(find "$CACHE/bench-$kind" -name libbox3d.a | head -1)
  t 300 cc -std=c17 -O2 -DNDEBUG -ffp-contract=off $extra -DBENCH_NAME="\"c_$kind\"" \
    -I"$SRC/include" -I"$SRC/src" "$HERE/c/harness.c" "$lib" -lm -lpthread -o "$BUILD/c_$kind"
done

# The Rae builds: one generated C file, compiled with and without the scalar
# Float4 lowering.
RAE="$RAE_ROOT/compiler/bin/rae"
RAE_CFLAGS="-std=gnu11 -O2 -DNDEBUG -ffp-contract=off -mllvm -inline-threshold=400 ${RAE_BENCH_CFLAGS:-}"
LINK="-framework Foundation -framework ImageIO -framework CoreGraphics"
t 300 "$RAE" build --target compiled --profile release --emit-c --out "$BUILD/rae.c" "$HERE/rae/Main.rae" >/dev/null
t 300 cc $RAE_CFLAGS -DRAE_FLOAT4_SCALAR "$BUILD/rae.c" "$BUILD/rae_runtime.c" -I"$BUILD" $LINK -o "$BUILD/rae_scalar"
t 300 cc $RAE_CFLAGS "$BUILD/rae.c" "$BUILD/rae_runtime.c" -I"$BUILD" $LINK -o "$BUILD/rae_simd"

best() {
  exe=$1; best_ns=""; line=""
  for _ in 1 2 3 4 5; do
    line=$(t 120 "$exe" | grep '^RESULT,')
    ns=$(echo "$line" | cut -d, -f3)
    if [ -z "$best_ns" ] || [ "$ns" -lt "$best_ns" ]; then best_ns=$ns; fi
  done
  echo "$(echo "$line" | cut -d, -f2) $best_ns $(echo "$line" | cut -d, -f4)"
}
C_SCALAR=$(best "$BUILD/c_scalar")
C_SIMD=$(best "$BUILD/c_simd")
RAE_SCALAR=$(best "$BUILD/rae_scalar")
RAE_SIMD=$(best "$BUILD/rae_simd")
for line in "$C_SCALAR" "$C_SIMD" "$RAE_SCALAR" "$RAE_SIMD"; do
  echo "$line" | awk '{ printf "%-12s %8.1f ms  checksum %s\n", $1, $2 / 1e6, $3 }'
done
echo "$C_SCALAR $C_SIMD $RAE_SCALAR $RAE_SIMD" | awk '{
  printf "checksums               %s\n", ($3 == $6 && $6 == $9 && $9 == $12) ? "match (bit-exact)" : "DIFFER";
  printf "c_scalar / c_simd       %.2fx\n", $2 / $5;
  printf "rae_scalar / c_scalar   %.2fx\n", $8 / $2;
  printf "%s / c_simd      %.2fx\n", $10, $11 / $5;
  printf "rae_scalar / %s  %.2fx\n", $10, $8 / $11;
}'
