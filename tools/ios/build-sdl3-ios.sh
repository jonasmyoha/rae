#!/bin/sh
# Build tools/ios/frameworks/SDL3.xcframework for iOS (#519).
#
# Homebrew's SDL3 is macOS-only. SDL does not ship an iOS xcframework, so we build
# the matching tag (release-3.4.10, = the homebrew version) from source with CMake's
# Xcode generator, for device (arm64) + simulator (arm64), and package an
# xcframework. Static only (App Store rules; no dylibs).
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out="$here/frameworks"
tag="release-3.4.10"
src="$here/.build/SDL"
b="$here/.build/sdl-ios"
depmin="13.0"

mkdir -p "$out" "$here/.build"

if [ ! -d "$src/.git" ]; then
  echo "sdl3-ios: cloning $tag..."
  rm -rf "$src"
  git clone --depth 1 --branch "$tag" https://github.com/libsdl-org/SDL.git "$src"
fi

echo "sdl3-ios: configuring (Xcode generator, iOS arm64)..."
cmake -S "$src" -B "$b" -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$depmin" \
  -DSDL_STATIC=ON -DSDL_SHARED=OFF \
  -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF \
  -DSDL_EXAMPLES=OFF

echo "sdl3-ios: building device (iphoneos)..."
cmake --build "$b" --config Release -- -sdk iphoneos -quiet
echo "sdl3-ios: building simulator (iphonesimulator)..."
cmake --build "$b" --config Release -- -sdk iphonesimulator -quiet

deva="$b/Release-iphoneos/libSDL3.a"
sima="$b/Release-iphonesimulator/libSDL3.a"
[ -f "$deva" ] || { echo "error: $deva not built" >&2; find "$b" -name 'libSDL3*.a' >&2; exit 1; }
[ -f "$sima" ] || { echo "error: $sima not built" >&2; find "$b" -name 'libSDL3*.a' >&2; exit 1; }

rm -rf "$out/SDL3.xcframework"
xcodebuild -create-xcframework \
  -library "$deva" -headers "$src/include" \
  -library "$sima" -headers "$src/include" \
  -output "$out/SDL3.xcframework"

echo "sdl3-ios: wrote $out/SDL3.xcframework"
