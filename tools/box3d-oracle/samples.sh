#!/bin/sh
# tools/box3d-oracle/samples.sh: build Box3D's own samples app (the oracle
# cache's source at 9f998c8) with a timing patch, and run one sample for a
# fixed number of frames (docs/physics-performance-plan.md §10). The patch
# (samples-timing.patch) adds three things:
#   SAMPLES_WORKERS=<n>                      the worker count (default cores / 2)
#   SAMPLES_WIDTH=<px> SAMPLES_HEIGHT=<px>   the window size (default 1920 x 1080)
#   one RAEPERF line at exit: the sample's step, Box3D's own world step
#   (b3World_GetProfile), the frame and the drawable wait, averaged over the
#   frames after the first 120
#
#   sh tools/box3d-oracle/samples.sh [sampleIndex] [frames]
#
# Sample 11 is "Benchmark / Large Pyramid" (the samples are sorted by category,
# then name). The Command Line Tools' clang does not find the SDK's libc++
# headers on its own here, so they are passed with -isystem.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CACHE="${RAE_BOX3D_CACHE:-$HOME/.cache/rae/box3d-oracle}"
SRC="$CACHE/samples-src"
BUILD="$CACHE/samples-build"
SAMPLE="${1:-11}"
FRAMES="${2:-1320}"
t() { seconds=$1; shift; perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"; }
if [ ! -x "$BUILD/bin/samples" ]; then
  rm -rf "$SRC"
  cp -R "$CACHE/box3d" "$SRC"
  (cd "$SRC" && patch -p1 < "$HERE/samples-timing.patch")
  SDK=$(xcrun --show-sdk-path)
  mkdir -p "$BUILD"
  (cd "$BUILD" && t 900 cmake "$SRC" -DCMAKE_BUILD_TYPE=Release -DBOX3D_SAMPLES=ON \
    -DBOX3D_BUILD_SHADERS=OFF -DBOX3D_UNIT_TESTS=OFF -DBOX3D_BENCHMARKS=OFF -DBOX3D_VALIDATE=OFF \
    -DCMAKE_CXX_FLAGS="-isystem $SDK/usr/include/c++/v1" >/dev/null)
  t 1800 cmake --build "$BUILD" --target samples -j 8 >/dev/null
fi
cd "$BUILD"
SAMPLES_WORKERS="${SAMPLES_WORKERS:-8}" SAMPLES_WIDTH="${SAMPLES_WIDTH:-1280}" SAMPLES_HEIGHT="${SAMPLES_HEIGHT:-800}" \
  t 600 ./bin/samples --sample "$SAMPLE" --frames "$FRAMES" 2>&1 | grep RAEPERF
