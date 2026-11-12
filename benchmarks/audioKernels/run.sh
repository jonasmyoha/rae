#!/bin/sh
# benchmarks/audioKernels: are Rae's audio loops as fast as C, and would C's
# `restrict` help? (docs/realtime-language-design.md §4.6). Builds the C
# reference (plain and `restrict`) and the Rae program, prints the best of
# five of each. RAE_BENCH_REMARKS=1 also prints clang's vectorizer verdict for
# each Rae kernel (-Rpass=loop-vectorize on the emitted C).
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAE_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
BUILD="$HERE/build"
mkdir -p "$BUILD"
t() { seconds=$1; shift; perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"; }
clang -O2 -ffp-contract=off "$HERE/c/reference.c" -o "$BUILD/reference"
t 300 "$BUILD/reference"
cd "$HERE/rae"
RAE_TOOLCHAIN_CHECK=off t 900 "$RAE_ROOT/compiler/bin/rae" run Main.rae 2>&1 | grep -E "^Rae"
if [ "${RAE_BENCH_REMARKS:-0}" = "1" ]; then
  RAE_TOOLCHAIN_CHECK=off t 300 "$RAE_ROOT/compiler/bin/rae" build --target compiled --emit-c --out "$BUILD/kernels.c" Main.rae >/dev/null
  include=$(ls -td "$HOME"/.rae/packages/audio/*/install/include | head -1)
  clang -O2 -ffp-contract=off -w -c "$BUILD/kernels.c" -I"$RAE_ROOT/compiler/runtime" -I"$include" -I/opt/homebrew/include \
    -o "$BUILD/kernels.o" -Rpass=loop-vectorize -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize 2> "$BUILD/remarks.txt" || true
  for kernel in renderPart sumLists sumBuffers sincDot; do
    start=$(grep -n "^RAE_UNUSED static [a-z_]* \*\{0,1\}rae_[A-Za-z_]*${kernel}_.*) {$" "$BUILD/kernels.c" | head -1 | cut -d: -f1)
    [ -n "$start" ] || continue
    end=$(awk -v s="$start" 'NR > s && /^}/ { print NR; exit }' "$BUILD/kernels.c")
    echo "$kernel:"
    grep -oE "kernels.c:[0-9]+:[0-9]+: remark: .*" "$BUILD/remarks.txt" | awk -F: -v s="$start" -v e="$end" '$2 >= s && $2 <= e' \
      | sed 's/^kernels.c:[0-9]*:[0-9]*: remark: /  /' | sort -u
  done
fi
