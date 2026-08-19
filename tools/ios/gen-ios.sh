#!/bin/sh
# Generate an iOS Xcode project for ONE Rae example (#520; seed of the multi-target
# generator #521). Reuses the compiler's `--emit-c`, links the two iOS xcframeworks
# (build them first: build-wgpu-ios.sh, build-sdl3-ios.sh), bundles the example's
# assets preserving relative paths, and produces build/ios/<name>/RaeApp.xcodeproj.
#
#   sh tools/ios/gen-ios.sh <example-dir> [bundle-id] [display-name]
#   e.g. sh tools/ios/gen-ios.sh examples/102_gpu2d_animated
#
# Then: open build/ios/<name>/RaeApp.xcodeproj, pick your team (Signing &
# Capabilities → Automatic), select the device, and Run. (Signing needs you —
# it can't be scripted headlessly.)
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
ex_arg=${1:?usage: gen-ios.sh <example-dir> [bundle-id] [name]}
if [ -d "$root/$ex_arg" ]; then
  ex_dir=$(CDPATH= cd -- "$root/$ex_arg" && pwd)
elif [ -d "$ex_arg" ]; then
  ex_dir=$(CDPATH= cd -- "$ex_arg" && pwd)
else
  echo "error: example dir not found: $ex_arg" >&2; exit 1
fi
name=$(basename "$ex_dir")
bundle_id=${2:-com.rae.$(printf '%s' "$name" | tr -cd 'a-zA-Z0-9')}
disp=${3:-$name}

rae="$root/compiler/bin/rae"
runtime_dir="$root/compiler/runtime"
frameworks="$here/frameworks"
gen="$root/build/ios/$name"
entry="$ex_dir/main.rae"

[ -x "$rae" ] || { echo "error: build the compiler first ($rae)" >&2; exit 1; }
[ -f "$entry" ] || { echo "error: no main.rae in $ex_dir" >&2; exit 1; }
[ -d "$frameworks/SDL3.xcframework" ] || { echo "error: SDL3.xcframework missing — run tools/ios/build-sdl3-ios.sh" >&2; exit 1; }
[ -d "$frameworks/wgpu_native.xcframework" ] || { echo "error: wgpu_native.xcframework missing — run tools/ios/build-wgpu-ios.sh" >&2; exit 1; }

mkdir -p "$gen"

# 1. Emit the app C, then prepend <SDL3/SDL_main.h> so the generated `int main`
#    becomes SDL_main and SDL owns the iOS UIApplication entry.
echo "gen-ios: emitting C for $name..."
"$rae" build --entry "$entry" --out "$gen/app.c" --target compiled --emit-c
{ printf '#include <SDL3/SDL_main.h>\n'; cat "$gen/app.c"; } > "$gen/app_ios.c"

# 2. Enumerate bundled assets (the example's assets/ tree). Each file is added to
#    the bundle at the SAME relative path so the emitted relative reads resolve
#    after the chdir shim.
asset_lines=""
if [ -d "$ex_dir/assets" ]; then
  while IFS= read -r f; do
    rel=${f#"$ex_dir/"}                 # e.g. assets/Roboto-Regular.mtsdf.json
    reldir=$(dirname "$rel")            # e.g. assets
    asset_lines="${asset_lines}  \"$f\"\n"
    pkg_lines="${pkg_lines:-}set_source_files_properties(\"$f\" PROPERTIES MACOSX_PACKAGE_LOCATION \"$reldir\")\n"
  done <<EOF
$(find "$ex_dir/assets" -type f)
EOF
fi

# 3. Emit CMakeLists.
cat > "$gen/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.28)
project(RaeApp C)

set(APP RaeApp)
add_executable(\${APP} MACOSX_BUNDLE
  "$gen/app_ios.c"
  "$runtime_dir/rae_runtime.c"
  "$here/ios_bootstrap.c"
$(printf "%b" "$asset_lines"))

$(printf "%b" "${pkg_lines:-}")

target_compile_definitions(\${APP} PRIVATE RAE_HAS_SDL3 RAE_HAS_WEBGPU)
target_compile_options(\${APP} PRIVATE -w)
target_include_directories(\${APP} PRIVATE
  "$runtime_dir"
  "$frameworks/SDL3.xcframework/ios-arm64/Headers"
  "$frameworks/SDL3.xcframework/ios-arm64-simulator/Headers"
  "$frameworks/wgpu_native.xcframework/ios-arm64/Headers"
  "$frameworks/wgpu_native.xcframework/ios-arm64-simulator/Headers")

target_link_libraries(\${APP}
  "$frameworks/SDL3.xcframework"
  "$frameworks/wgpu_native.xcframework"
  "-framework UIKit" "-framework Foundation" "-framework CoreFoundation"
  "-framework Metal" "-framework QuartzCore" "-framework CoreGraphics"
  "-framework ImageIO" "-framework CoreMotion" "-framework GameController"
  "-framework AVFoundation" "-framework AudioToolbox" "-framework CoreHaptics"
  "-framework CoreText" "-framework UniformTypeIdentifiers"
  "-framework CoreBluetooth" "-framework CoreMedia" "-framework CoreVideo"
  "-framework OpenGLES" "-framework Security")

set_target_properties(\${APP} PROPERTIES
  MACOSX_BUNDLE_GUI_IDENTIFIER "$bundle_id"
  MACOSX_BUNDLE_BUNDLE_NAME "$disp"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "$bundle_id"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
  XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
  XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "13.0")
EOF

# 4. Configure the Xcode project.
echo "gen-ios: configuring Xcode project..."
cmake -S "$gen" -B "$gen/proj" -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 >/dev/null

echo ""
echo "gen-ios: project at $gen/proj/RaeApp.xcodeproj"
echo "  open '$gen/proj/RaeApp.xcodeproj'"
echo "  → Signing & Capabilities: pick your Team (Automatic), select the device, Run."
