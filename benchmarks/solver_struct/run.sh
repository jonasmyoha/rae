#!/bin/sh
# benchmarks/solver_struct: Rae's safe List accessors on solver-sized structs
# against the same C, plain and bounds-checked (see rae/Main.rae). Prints the
# best of five runs of each and the ratios. Same build flags as list_access:
# -O2 -DNDEBUG -ffp-contract=off (C plain at -O3, as list_access's C baseline).
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAE_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
BUILD="$HERE/build"
mkdir -p "$BUILD"
t() { seconds=$1; shift; perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"; }

LOAD=$(uptime | sed 's/.*load averages*:[ ]*//' | awk '{print $1}')
CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
if [ "${BENCH_ALLOW_LOAD:-0}" != "1" ] && awk "BEGIN{exit !($LOAD > $CORES * 0.3)}"; then
  echo "refusing to benchmark: load average $LOAD on $CORES cores (BENCH_ALLOW_LOAD=1 overrides)." >&2
  exit 1
fi

t 300 "$RAE_ROOT/compiler/bin/rae" build --target compiled --profile release --emit-c \
  --out "$BUILD/rae_generated.c" "$HERE/rae/Main.rae" >/dev/null
t 300 cc -std=c11 -O2 -DNDEBUG -ffp-contract=off "$BUILD/rae_generated.c" "$BUILD/rae_runtime.c" \
  -I"$BUILD" -framework Foundation -framework ImageIO -framework CoreGraphics -o "$BUILD/rae_solver_struct"
t 120 cc -std=c11 -O3 -DNDEBUG -ffp-contract=off "$HERE/c/solver_struct.c" -o "$BUILD/c_solver_struct"
t 120 cc -std=c11 -O3 -DNDEBUG -ffp-contract=off -DCHECKED "$HERE/c/solver_struct.c" -o "$BUILD/c_checked_solver_struct"

best() {
  exe=$1; best_ns=""; line=""
  for _ in 1 2 3 4 5; do
    line=$(t 120 "$exe" | grep '^RESULT,')
    ns=$(echo "$line" | cut -d, -f3)
    if [ -z "$best_ns" ] || [ "$ns" -lt "$best_ns" ]; then best_ns=$ns; fi
  done
  echo "$(echo "$line" | cut -d, -f2) $best_ns $(echo "$line" | cut -d, -f4)"
}
RAE=$(best "$BUILD/rae_solver_struct")
C=$(best "$BUILD/c_solver_struct")
CHECKED=$(best "$BUILD/c_checked_solver_struct")
echo "$RAE"; echo "$C"; echo "$CHECKED"
echo "$RAE $C $CHECKED" | awk '{
  printf "checksums %s\n", ($3 == $6 && $6 == $9) ? "match" : "DIFFER";
  printf "rae / c         %.2fx\n", $2 / $5;
  printf "rae / c_checked %.2fx\n", $2 / $8;
}'
