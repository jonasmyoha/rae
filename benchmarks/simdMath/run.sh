#!/bin/sh
# benchmarks/simdMath: Float4 / Float8 sin, exp and tanh (lib/Float4.rae)
# against looping Math's scalar ones, with the platform's SIMD and with the
# scalar lowering (--float4-scalar). Best of five after a warm-up.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAE_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
cd "$HERE"
for flag in "" "--float4-scalar"; do
  RAE_TOOLCHAIN_CHECK=off perl -e 'alarm shift; exec @ARGV' 600 "$RAE_ROOT/compiler/bin/rae" run $flag Main.rae 2>&1 | grep -v "^@@"
done
