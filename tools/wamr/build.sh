#!/bin/bash
# Build WAMR for lib/wasm (docs/wasm-embedding.md).
#
# usage: tools/wamr/build.sh
#
#   - Clones the pinned WAMR (tools/wamr/pin, Apache-2.0 with LLVM exception)
#     into a cache OUTSIDE the repo ($RAE_WAMR_CACHE, default
#     ~/.cache/rae/wamr). No WAMR source enters this repository.
#   - Builds its fast interpreter (no AOT, no JIT) with lib/wasm's glue
#     (tools/wamr/rae_wasm_glue.c) into the install directory ($RAE_WAMR,
#     default <cache>/install), which `rae` links when a program imports
#     wasm/...:
#       lib/librae_wamr.a               this machine (macOS or Linux)
#       lib/ios/librae_wamr.a           iPhone (arm64), when Xcode is there
#       lib/ios-simulator/librae_wamr.a the iOS simulator (arm64)
#     plus COMMIT. The glue's prototypes are compiler/runtime/runtime_wasm_embed.h.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/pin"
CACHE="${RAE_WAMR_CACHE:-$HOME/.cache/rae/wamr}"
SRC="$CACHE/src"
INSTALL="${RAE_WAMR:-$CACHE/install}"
start=$(date +%s)

mkdir -p "$CACHE"
if [ ! -d "$SRC/.git" ]; then
  git clone --quiet --depth 1 --branch "$WAMR_TAG" "$WAMR_URL" "$SRC"
fi
if [ "$(git -C "$SRC" rev-parse --short=9 HEAD)" != "$WAMR_COMMIT" ]; then
  git -C "$SRC" fetch --quiet --depth 1 origin "refs/tags/$WAMR_TAG:refs/tags/$WAMR_TAG"
  git -C "$SRC" checkout --quiet --detach "$WAMR_TAG"
fi

rm -rf "${INSTALL:?}"
mkdir -p "$INSTALL/lib"

# build <out dir> <cmake options...>
build() {
  out="$1"; shift
  work="$(mktemp -d)"
  cmake -S "$HERE" -B "$work" -DWAMR_ROOT_DIR="$SRC" -DCMAKE_BUILD_TYPE=Release "$@" > "$work/configure.log" 2>&1 \
    || { tail -20 "$work/configure.log" >&2; exit 1; }
  cmake --build "$work" --target rae_wamr -j 8 > "$work/build.log" 2>&1 \
    || { tail -30 "$work/build.log" >&2; exit 1; }
  mkdir -p "$out"
  cp "$work/librae_wamr.a" "$out/"
  rm -rf "${work:?}"
}

case "$(uname -s)" in
  Darwin) platform=darwin ;;
  Linux) platform=linux ;;
  *) echo "wamr: no WAMR platform for $(uname -s)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
  arm64|aarch64) target=AARCH64 ;;
  x86_64|amd64) target=X86_64 ;;
  *) echo "wamr: no WAMR target for $(uname -m)" >&2; exit 1 ;;
esac
build "$INSTALL/lib" -DWAMR_BUILD_PLATFORM=$platform -DWAMR_BUILD_TARGET=$target

if [ "$platform" = darwin ]; then
  developer="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
  if [ -d "$developer" ]; then
    export DEVELOPER_DIR="$developer"
    build "$INSTALL/lib/ios" -DWAMR_BUILD_PLATFORM=darwin -DWAMR_BUILD_TARGET=AARCH64 \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
    build "$INSTALL/lib/ios-simulator" -DWAMR_BUILD_PLATFORM=darwin -DWAMR_BUILD_TARGET=AARCH64 \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
  else
    echo "wamr: Xcode not found at $developer; skipped the iOS libraries"
  fi
fi
echo "$WAMR_COMMIT" > "$INSTALL/COMMIT"

end=$(date +%s)
echo "wamr: $WAMR_TAG built into $INSTALL in $((end - start)) s"
for lib in "$INSTALL"/lib/librae_wamr.a "$INSTALL"/lib/*/librae_wamr.a; do
  [ -f "$lib" ] && echo "  ${lib#"$INSTALL"/}: $(( $(wc -c < "$lib") / 1024 )) KiB"
done
