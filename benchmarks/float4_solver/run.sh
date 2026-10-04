#!/bin/sh
# benchmarks/float4_solver: the wide convex contact kernels (warm start, push,
# relax) four ways on the same synthetic constraints (docs/float4-design.md):
#
#   c_scalar     Box3D's contact_solver.c, BOX3D_DISABLE_SIMD
#   c_simd       Box3D's contact_solver.c, NEON / SSE2
#   rae_scalar   lib/physics/dynamics (FloatWide: a struct of four Floats)
#   rae_float4   the same solver modules over the Float4 prototype: a copy
#                of FloatWide whose primitives are SIMD (float4/, and
#                c/float4_prototype.h force-included into the generated C)
#   rae_float4_vector  the same, with FloatWide a vector type in the C
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

# The Rae builds
RAE="$RAE_ROOT/compiler/bin/rae"
RAE_CFLAGS="-std=gnu11 -O2 -DNDEBUG -ffp-contract=off -mllvm -inline-threshold=400 ${RAE_BENCH_CFLAGS:-}"
LINK="-framework Foundation -framework ImageIO -framework CoreGraphics"
t 300 "$RAE" build --target compiled --profile release --emit-c --out "$BUILD/rae_scalar.c" "$HERE/rae/Main.rae" >/dev/null
t 300 cc $RAE_CFLAGS "$BUILD/rae_scalar.c" "$BUILD/rae_runtime.c" -I"$BUILD" $LINK -o "$BUILD/rae_scalar"

# The float4 project: the solver modules that name FloatWide, copied from
# lib/ into a package of their own so they all compile against the
# prototype's FloatWide and not the lib's.
PROJECT="$BUILD/float4_project"
rm -rf "$PROJECT"
mkdir -p "$PROJECT/wide"
for module in ContactConstraints StepContext ContactSolverWide ContactSolverWideSolve; do
  cp "$RAE_ROOT/lib/physics/dynamics/$module.rae" "$PROJECT/wide/"
done
cp "$HERE/float4/physics/dynamics/FloatWide.rae" "$PROJECT/wide/"
sed 's/ret "rae_scalar"/ret "rae_float4"/' "$HERE/rae/Main.rae" > "$PROJECT/Main.rae"
# Inside the repository, module paths are relative to its root
PACKAGE="${PROJECT#$RAE_ROOT/}/wide"
for file in "$PROJECT"/wide/*.rae "$PROJECT/Main.rae"; do
  sed -E "s#physics/dynamics/(FloatWide|ContactConstraints|StepContext|ContactSolverWide|ContactSolverWideSolve)\$#$PACKAGE/\\1#" \
    "$file" > "$file.tmp" && mv "$file.tmp" "$file"
done
t 300 "$RAE" build --target compiled --profile release --emit-c --out "$BUILD/rae_float4.c" "$PROJECT/Main.rae" >/dev/null
t 300 cc $RAE_CFLAGS -include "$HERE/c/float4_prototype.h" "$BUILD/rae_float4.c" "$BUILD/rae_runtime.c" \
  -I"$BUILD" $LINK -o "$BUILD/rae_float4"

# rae_float4_vector: the same generated C with FloatWide retyped as a clang
# vector (ext_vector_type keeps the .x .y .z .w fields): what a builtin
# Float4 would emit, so the values stay in vector registers between the
# primitives instead of passing through a struct in memory.
perl -0pe 's/typedef struct rae_FloatWide rae_FloatWide;/typedef float rae_FloatWide __attribute__((ext_vector_type(4)));/; s/struct rae_FloatWide \{\n  float x;\n  float y;\n  float z;\n  float w;\n\};\n//' \
  "$BUILD/rae_float4.c" > "$BUILD/rae_float4_vector.c"
t 300 cc $RAE_CFLAGS -include "$HERE/c/float4_prototype.h" "$BUILD/rae_float4_vector.c" "$BUILD/rae_runtime.c" \
  -I"$BUILD" $LINK -o "$BUILD/rae_float4_vector"

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
RAE_FLOAT4=$(best "$BUILD/rae_float4")
RAE_VECTOR=$(best "$BUILD/rae_float4_vector" | sed 's/^rae_float4 /rae_float4_vector /')
for line in "$C_SCALAR" "$C_SIMD" "$RAE_SCALAR" "$RAE_FLOAT4" "$RAE_VECTOR"; do
  echo "$line" | awk '{ printf "%-17s %8.1f ms  checksum %s\n", $1, $2 / 1e6, $3 }'
done
echo "$C_SCALAR $C_SIMD $RAE_SCALAR $RAE_FLOAT4 $RAE_VECTOR" | awk '{
  printf "checksums             %s\n", ($3 == $6 && $6 == $9 && $9 == $12 && $12 == $15) ? "match (bit-exact)" : "DIFFER";
  printf "c_scalar / c_simd     %.2fx\n", $2 / $5;
  printf "rae_scalar / c_scalar %.2fx\n", $8 / $2;
  printf "rae_float4 / c_simd   %.2fx\n", $11 / $5;
  printf "rae_scalar / rae_float4 %.2fx\n", $8 / $11;
  printf "rae_float4_vector / c_simd %.2fx\n", $14 / $5;
  printf "rae_scalar / rae_float4_vector %.2fx\n", $8 / $14;
}'
