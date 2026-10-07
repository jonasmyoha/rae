#!/bin/bash
# The Box3D oracle (docs/physics-rae-port-design.md §4): Box3D's own C, built
# the way the Rae port must behave, writes golden traces the port's fixtures
# compare against bit for bit.
#
# usage: tools/box3d-oracle/oracle.sh [driver ...]     (default: every driver)
#
#   - Clones github.com/erincatto/box3d into a cache OUTSIDE the repo
#     ($RAE_BOX3D_CACHE, default ~/.cache/rae/box3d-oracle) and checks out the
#     port's frozen commit (tools/box3d-oracle/pin). No Box3D source ever enters this repository.
#   - Builds the library scalar (BOX3D_DISABLE_SIMD), with -ffp-contract=off
#     (Box3D's CMake adds it too), validation and every extra target off.
#   - Compiles each tools/box3d-oracle/drivers/<name>.c against it (upstream's
#     shared/ sample code is on the include path: a driver may #include e.g.
#     human.c), runs it,
#     and writes tools/box3d-oracle/goldens/<name>.golden: one line per call,
#     every float a C hex float (`%a`), which is exact and parses back to the
#     same bits. A driver that steps a world creates it with workerCount = 1.
#
# The port is frozen at tools/box3d-oracle/pin (docs/physics-two-implementations.md
# §7); the C-library track moves tools/box3d/pin on its own. Re-porting is its
# own decision: change this pin, rerun, and let the bit-exact fixtures show
# every place the behaviour changed.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/pin"
CACHE="${RAE_BOX3D_CACHE:-$HOME/.cache/rae/box3d-oracle}"
SRC="$CACHE/box3d"
BUILD="$CACHE/build-$BOX3D_COMMIT"

mkdir -p "$CACHE" "$HERE/goldens"
if [ ! -d "$SRC/.git" ]; then
  git clone --quiet "$BOX3D_URL" "$SRC"
fi
if ! git -C "$SRC" cat-file -e "$BOX3D_COMMIT^{commit}" 2>/dev/null; then
  git -C "$SRC" fetch --quiet origin
fi
git -C "$SRC" checkout --quiet --detach "$BOX3D_COMMIT"

cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DBOX3D_DISABLE_SIMD=ON -DBOX3D_VALIDATE=OFF -DBOX3D_PROFILE=OFF \
  -DBOX3D_SAMPLES=OFF -DBOX3D_UNIT_TESTS=OFF -DBOX3D_BENCHMARKS=OFF -DBOX3D_DOCS=OFF \
  -DCMAKE_C_FLAGS="-ffp-contract=off" > "$BUILD.configure.log" 2>&1 \
  || { echo "box3d-oracle: cmake configure failed (see $BUILD.configure.log)"; exit 1; }
cmake --build "$BUILD" --target box3d --parallel > "$BUILD.build.log" 2>&1 \
  || { echo "box3d-oracle: build failed (see $BUILD.build.log)"; exit 1; }
LIB="$(find "$BUILD" -name 'libbox3d.a' | head -1)"
[ -n "$LIB" ] || { echo "box3d-oracle: libbox3d.a not found under $BUILD"; exit 1; }

DRIVERS=("$@")
if [ ${#DRIVERS[@]} -eq 0 ]; then
  for path in "$HERE"/drivers/*.c; do DRIVERS+=("$(basename "$path" .c)"); done
fi
for driver in "${DRIVERS[@]}"; do
  exe="$BUILD/oracle-$driver"
  cc -std=c17 -O2 -DNDEBUG -ffp-contract=off -DBOX3D_DISABLE_SIMD \
    -DBOX3D_ORACLE_COMMIT="\"$BOX3D_COMMIT\"" -I"$SRC/include" -I"$SRC/src" -I"$SRC/shared" \
    "$HERE/drivers/$driver.c" "$LIB" -lm -lpthread -o "$exe"
  "$exe" > "$HERE/goldens/$driver.golden"
  echo "box3d-oracle: $driver -> tools/box3d-oracle/goldens/$driver.golden ($(wc -l < "$HERE/goldens/$driver.golden" | tr -d ' ') lines)"
done
