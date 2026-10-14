#!/bin/sh
# The forward + MSAA study's measurements (docs/forward-msaa-study.md) on this
# machine: the geometry-heavy scene at 1920x1080 and 3840x2160, the light one
# at 2560x1440 and 3840x2160, timed by the clock with 64 frames per submit.
# FM_IMAGES=<dir> also saves one PNG per variant.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAE_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
for config in "30 1920x1080" "30 3840x2160" "12 2560x1440" "12 3840x2160"; do
  set -- $config
  echo "== $1 x $1 spheres, $2"
  RAE_FM_TIMESTAMPS=0 RAE_FM_BATCH=64 RAE_FM_FRAMES=60 RAE_FM_SIDE=$1 RAE_FM_SIZE=$2 \
    RAE_FM_IMAGES="${FM_IMAGES:-}" \
    perl -e 'alarm shift; exec @ARGV' 300 "$RAE_ROOT/compiler/bin/rae" run "$HERE/Main.rae" 2>&1 \
    | grep -E 'study|: gpu|rror' | sed 's/gpu -1 us (first pass -1 us), //'
done
