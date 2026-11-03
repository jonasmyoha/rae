#!/bin/sh
# Download wgpu-native for Android arm64 into tools/android/deps/wgpu: the
# pinned release (the version lib/webgpu/*.rae was generated against, the one
# in ~/.local/wgpu-native) publishes Android builds, so nothing is compiled.
# wgpu draws through Vulkan on Android.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$here/env.sh"
tag=$(cat "$HOME/.local/wgpu-native/wgpu-native-meta/wgpu-native-git-tag")
out="$deps/wgpu"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

echo "wgpu-android: downloading $tag (aarch64)..."
curl -fsSL "https://github.com/gfx-rs/wgpu-native/releases/download/$tag/wgpu-android-aarch64-release.zip" -o "$work/wgpu.zip"
unzip -q "$work/wgpu.zip" -d "$work/wgpu"
rm -rf "$out"; mkdir -p "$out/lib"
library=$(find "$work/wgpu" -name 'libwgpu_native.a' | head -1)
[ -n "$library" ] || { echo "error: libwgpu_native.a not in the download" >&2; find "$work/wgpu" >&2; exit 1; }
cp "$library" "$out/lib/"
headers=$(find "$work/wgpu" -type d -name include | head -1)
cp -R "$headers" "$out/include"
echo "$tag" > "$out/tag"
echo "wgpu-android: wrote $out"
