#!/bin/sh
# Build tools/ios/frameworks/wgpu_native.xcframework for iOS (#518).
#
# ~/.local/wgpu-native is macOS-only. The pinned release (v29.0.1.1 — the version
# the checked-in lib/webgpu/*.rae bindings were generated against) publishes
# prebuilt iOS static libs, so we DOWNLOAD the matching-version libs rather than
# building the Rust source. Headers come from the local install (same version).
#
# Produces an xcframework with device (arm64) + simulator (arm64) slices.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
out="$here/frameworks"
hdr_src="$HOME/.local/wgpu-native/include"     # webgpu/webgpu.h + webgpu/wgpu.h, v29.0.1.1
tag=$(cat "$HOME/.local/wgpu-native/wgpu-native-meta/wgpu-native-git-tag")
base="https://github.com/gfx-rs/wgpu-native/releases/download/${tag}"

if [ ! -d "$hdr_src/webgpu" ]; then
  echo "error: wgpu headers not found at $hdr_src/webgpu" >&2; exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM
mkdir -p "$out" "$work/headers/webgpu"
cp "$hdr_src"/webgpu/*.h "$work/headers/webgpu/"

echo "wgpu-ios: downloading $tag device + simulator (arm64) libs..."
curl -fsSL "$base/wgpu-ios-aarch64-release.zip"           -o "$work/dev.zip"
curl -fsSL "$base/wgpu-ios-aarch64-simulator-release.zip" -o "$work/sim.zip"
unzip -q "$work/dev.zip" -d "$work/dev"
unzip -q "$work/sim.zip" -d "$work/sim"

deva=$(find "$work/dev" -name 'libwgpu_native.a' | head -1)
sima=$(find "$work/sim" -name 'libwgpu_native.a' | head -1)
if [ -z "$deva" ] || [ -z "$sima" ]; then
  echo "error: libwgpu_native.a not found in downloaded zips" >&2
  find "$work/dev" "$work/sim" -name '*.a' >&2; exit 1
fi
echo "wgpu-ios: device=$deva"
echo "wgpu-ios: sim=$sima"

rm -rf "$out/wgpu_native.xcframework"
xcodebuild -create-xcframework \
  -library "$deva" -headers "$work/headers" \
  -library "$sima" -headers "$work/headers" \
  -output "$out/wgpu_native.xcframework"

echo "wgpu-ios: wrote $out/wgpu_native.xcframework"
