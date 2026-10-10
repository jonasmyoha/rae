#!/bin/sh
# benchmarks/float8_kernel: a Horner polynomial over 1M Floats, scalar vs
# Float4 vs Float8 (docs/float4-design.md §10). Builds the Rae program
# release twice: as `rae` builds for this machine (AVX2 on an x86-64 CPU that
# has it) and with lib/Float4/Float8 forced onto their scalar definitions.
# Prints the best of 20 rounds of each form, in ms, and checks that every
# form and build prints the same checksum. On x86-64, RAE_X86_AVX2=0 builds
# without AVX2 for the comparison.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAE_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
RAE="$RAE_ROOT/compiler/bin/rae"
BUILD="$HERE/build"
mkdir -p "$BUILD"
t() { seconds=$1; shift; perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"; }

LOAD=$(uptime | sed 's/.*load averages*:[ ]*//' | awk '{print $1}')
CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
if [ "${BENCH_ALLOW_LOAD:-0}" != "1" ] && awk "BEGIN{exit !($LOAD > $CORES * 0.3)}"; then
  echo "refusing to benchmark: load average $LOAD on $CORES cores (BENCH_ALLOW_LOAD=1 overrides)." >&2
  exit 1
fi
echo "machine: $(uname -m), $(sysctl -n machdep.cpu.brand_string 2>/dev/null || grep -m1 'model name' /proc/cpuinfo | cut -d: -f2), load $LOAD"
for build in native scalar; do
  flag=""
  [ "$build" = scalar ] && flag="--float4-scalar"
  # `rae run` compiles as every Rae program is compiled (release flags, and
  # -mavx2 on an x86-64 CPU that has it), then runs it
  t 600 "$RAE" run --profile release $flag "$HERE/rae/Main.rae" 2>/dev/null | grep '^RESULT' \
    | sed "s/^RESULT,/$build,/" >> "$BUILD/results.$$"
done
awk -F, '
  { printf "%-7s %-16s %8.2f ms  checksum %s\n", $1, $2, $3 / 1e6, $4; ms[$1 "," $2] = $3; sums[$4] = 1 }
  END {
    n = 0; for (s in sums) n++
    printf "checksums: %s\n", (n == 1) ? "all match" : "DIFFER"
    for (k in ms) if (k ~ /^native,float4/) f4 = ms[k]; else if (k ~ /^native,float8/) f8 = ms[k]; else if (k == "native,scalar") sc = ms[k]
    printf "native: scalar / float4 %.2fx, float4 / float8 %.2fx\n", sc / f4, f4 / f8
  }' "$BUILD/results.$$"
rm -f "$BUILD/results.$$"
