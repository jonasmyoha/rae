#!/bin/bash
# Build upstream Box3D's C library for the C-library physics track
# (docs/physics-two-implementations.md §4, lib/box3d).
#
# usage: tools/box3d/build.sh
#
#   - Clones github.com/erincatto/box3d into a cache OUTSIDE the repo
#     ($RAE_BOX3D_CACHE, default ~/.cache/rae/box3d) and checks out the pinned
#     commit (tools/box3d/pin). No Box3D source ever enters this repository.
#   - Compiles src/*.c with -ffp-contract=off (Box3D's cross-platform
#     determinism flag) into three static libraries under the install
#     directory ($RAE_BOX3D, default <cache>/install), which is what `rae`
#     links when a program imports box3d/...:
#       lib/libbox3d.a          native, SIMD on (NEON / SSE2): the product build
#       lib/scalar/libbox3d.a   native, BOX3D_DISABLE_SIMD: the oracle's build
#       lib/wasm32/libbox3d.a   wasm32-wasip1 via wasi-sdk ($WASI_SDK), -msimd128
#     plus include/box3d/*.h and COMMIT (the pin it was built from). Each
#     library also holds tools/box3d/rae_glue.c (the callbacks and event
#     array reads Rae cannot do itself yet; header installed as
#     include/box3d/rae_glue.h).
#   - Prints the build time and the libraries' sizes.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/pin"
CACHE="${RAE_BOX3D_CACHE:-$HOME/.cache/rae/box3d}"
SRC="$CACHE/src"
INSTALL="${RAE_BOX3D:-$CACHE/install}"
WASI_SDK="${WASI_SDK:-$HOME/.local/wasi-sdk}"
start=$(date +%s)

mkdir -p "$CACHE"
if [ ! -d "$SRC/.git" ]; then
  git clone --quiet "$BOX3D_URL" "$SRC"
fi
if ! git -C "$SRC" cat-file -e "$BOX3D_COMMIT^{commit}" 2>/dev/null; then
  git -C "$SRC" fetch --quiet origin
fi
git -C "$SRC" checkout --quiet --detach "$BOX3D_COMMIT"

rm -rf "$INSTALL"
mkdir -p "$INSTALL/include" "$INSTALL/lib/scalar" "$INSTALL/lib/wasm32" "$INSTALL/lib/emscripten"
cp -R "$SRC/include/box3d" "$INSTALL/include/"
cp "$HERE/rae_glue.h" "$INSTALL/include/box3d/rae_glue.h"

# build <cc> <ar> <out.a> <flags...>: every src/*.c into one archive
build() {
  cc="$1"; archiver="$2"; out="$3"; shift 3
  objs="$(mktemp -d)"
  for file in "$SRC"/src/*.c "$HERE/rae_glue.c"; do
    "$cc" -std=c17 -O2 -DNDEBUG -ffp-contract=off "$@" -I"$INSTALL/include" -I"$SRC/src" \
      -c "$file" -o "$objs/$(basename "$file" .c).o"
  done
  "$archiver" rcs "$out" "$objs"/*.o
  rm -rf "$objs"
}
build cc ar "$INSTALL/lib/libbox3d.a"
build cc ar "$INSTALL/lib/scalar/libbox3d.a" -DBOX3D_DISABLE_SIMD
if [ -x "$WASI_SDK/bin/clang" ]; then
  build "$WASI_SDK/bin/clang" "$WASI_SDK/bin/llvm-ar" "$INSTALL/lib/wasm32/libbox3d.a" \
    --target=wasm32-wasip1 --sysroot="$WASI_SDK/share/wasi-sysroot" -msimd128
else
  echo "box3d: wasi-sdk not found at $WASI_SDK; skipped the wasm32 library"
fi
# The browser build (rae build --target wasm links it with emcc)
if command -v emcc >/dev/null 2>&1; then
  # Emscripten needs python >= 3.10; Apple's /usr/bin python3 is 3.9 and may
  # come first on PATH. Pick a recent one unless EMSDK_PYTHON is already set
  # (the same rule `rae build --target wasm` applies).
  if [ -z "${EMSDK_PYTHON:-}" ]; then
    for candidate in /opt/homebrew/bin/python3 /usr/local/bin/python3 python3; do
      if "$candidate" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' 2>/dev/null; then
        export EMSDK_PYTHON="$candidate"; break
      fi
    done
  fi
  # Box3D takes its SSE2 path here; Emscripten lowers SSE2 to WASM SIMD
  build emcc emar "$INSTALL/lib/emscripten/libbox3d.a" -msimd128 -msse2 -D_POSIX_C_SOURCE=200809L
else
  echo "box3d: emcc not found; skipped the browser library"
fi
echo "$BOX3D_COMMIT" > "$INSTALL/COMMIT"

end=$(date +%s)
echo "box3d: $BOX3D_COMMIT built into $INSTALL in $((end - start)) s"
for lib in "$INSTALL"/lib/libbox3d.a "$INSTALL"/lib/*/libbox3d.a; do
  [ -f "$lib" ] && echo "  ${lib#$INSTALL/}: $(( $(wc -c < "$lib") / 1024 )) KiB"
done
