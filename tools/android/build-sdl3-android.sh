#!/bin/sh
# Build SDL3 for Android arm64 into tools/android/deps/sdl3: libSDL3.so, its
# headers, and the Java half (org.libsdl.app.*) the APK needs. Same tag as the
# iOS build and Homebrew's SDL (release-3.4.10). Shared, as SDL wants on
# Android: SDLActivity loads libSDL3.so and then the app's libmain.so.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$here/env.sh"
tag="release-3.4.10"
src="$here/.build/SDL"
build="$here/.build/sdl-android"
out="$deps/sdl3"

mkdir -p "$here/.build"
if [ ! -d "$src/.git" ]; then
  echo "sdl3-android: cloning $tag..."
  rm -rf "$src"
  git clone --depth 1 --branch "$tag" https://github.com/libsdl-org/SDL.git "$src"
fi

echo "sdl3-android: configuring (NDK $(basename "$ndk"), arm64-v8a, API $api)..."
cmake -S "$src" -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-$api" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF \
  -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF >/dev/null
echo "sdl3-android: building..."
cmake --build "$build" >/dev/null

rm -rf "$out"; mkdir -p "$out/lib" "$out/java"
cp "$build/libSDL3.so" "$out/lib/"
cp -R "$src/include" "$out/include"
cp -R "$src/android-project/app/src/main/java/org" "$out/java/"
echo "sdl3-android: wrote $out"
