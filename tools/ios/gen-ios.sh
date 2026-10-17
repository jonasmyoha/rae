#!/bin/sh
# Generate an iOS Xcode project for ONE Rae example (#520; seed of the multi-target
# generator #521). Reuses the compiler's `--emit-c`, links the two iOS xcframeworks
# (build them first: build-wgpu-ios.sh, build-sdl3-ios.sh), bundles the example's
# assets preserving relative paths, and produces build/ios/<name>/RaeApp.xcodeproj.
#
#   sh tools/ios/gen-ios.sh <project-dir> [bundle-id] [display-name]
#   e.g. sh tools/ios/gen-ios.sh examples/102_gpu2d_animated
#
# <project-dir> is an in-repo example (entry main.rae, assets read by repo-root
# path too) OR an out-of-tree game repo (entry src/main.rae, assets/ and lib/*
# read CWD-relative). Both are handled; RAE_IOS_ENTRY overrides the entry file.
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
# Bundle id / display name: positional args win, else RAE_IOS_BUNDLE_ID /
# RAE_IOS_NAME (so run-ios.sh, which regenerates with no positionals, can still
# carry a project's chosen id), else a default derived from the dir name.
bundle_id=${2:-${RAE_IOS_BUNDLE_ID:-com.rae.$(printf '%s' "$name" | tr -cd 'a-zA-Z0-9')}}
disp=${3:-${RAE_IOS_NAME:-$name}}
# The orientations the app turns to (Info.plist): `any` (the default: portrait
# and both landscapes, phone and iPad — the app follows how the device is held)
# or `portrait` (phone upright only, iPad either way up), for an app designed
# for one shape. SDL follows the device within them; the window is resizable,
# so the app sees a resize and re-lays out.
# The oldest iOS the app runs on. 15.0 is the oldest current Xcode builds for
# (Xcode 27 refuses anything older); RAE_IOS_DEPLOYMENT_TARGET overrides it.
deployment_target=${RAE_IOS_DEPLOYMENT_TARGET:-15.0}
# The app icon: RAE_IOS_ICON names a square PNG, 1024x1024 and opaque (the App
# Store refuses alpha). It becomes the AppIcon set of an asset catalog, in the
# single-size form (Xcode makes every other size from it). Unset, the app has
# the system's blank icon.
icon=${RAE_IOS_ICON:-}
if [ -n "$icon" ] && [ ! -f "$icon" ]; then
  echo "error: RAE_IOS_ICON is not a file: $icon" >&2; exit 1
fi
# The version the App Store shows (CFBundleShortVersionString, e.g. 0.1.0) and
# the build number (CFBundleVersion), which must grow with every upload of the
# same version.
version=${RAE_IOS_VERSION:-1.0}
build_number=${RAE_IOS_BUILD:-1}
printf '%s' "$version" | grep -Eq '^[0-9]+(\.[0-9]+){0,2}$' \
  || { echo "error: RAE_IOS_VERSION is one to three numbers with dots, not '$version'" >&2; exit 1; }
printf '%s' "$build_number" | grep -Eq '^[0-9]+(\.[0-9]+){0,2}$' \
  || { echo "error: RAE_IOS_BUILD is one to three numbers with dots, not '$build_number'" >&2; exit 1; }
# RAE_IOS_EXEMPT_ENCRYPTION=1 declares that the app uses no encryption beyond
# what is exempt (none, or the system's HTTPS): ITSAppUsesNonExemptEncryption
# NO, so App Store Connect stops asking at every upload. The app's own call.
extra_plist=""
if [ "${RAE_IOS_EXEMPT_ENCRYPTION:-0}" = "1" ]; then
  extra_plist='<key>ITSAppUsesNonExemptEncryption</key><false/>'
fi
# RAE_IOS_HIGH_REFRESH=1 lets the app present above 60 Hz on a ProMotion
# iPhone (CADisableMinimumFrameDurationOnPhone): iOS otherwise holds every
# app to 60. Off by default (it costs battery); used to measure GPU headroom
# that a 60 Hz cap hides (forward-rae-4).
if [ "${RAE_IOS_HIGH_REFRESH:-0}" = "1" ]; then
  extra_plist="$extra_plist<key>CADisableMinimumFrameDurationOnPhone</key><true/>"
fi
# Purpose strings. SDL3 links the camera (AVCaptureDevice), Bluetooth
# (CBCentralManager, for controllers) and microphone recording, and App Store
# Connect refuses a build whose binary references those without a purpose
# string in Info.plist (ITMS-90683): the build is dropped after upload and
# never reaches TestFlight. iOS shows one only if the app actually asks for
# that access. RAE_IOS_BLUETOOTH_USAGE / RAE_IOS_CAMERA_USAGE /
# RAE_IOS_MICROPHONE_USAGE word them for the app (plain text, no < & >).
bluetooth_usage=${RAE_IOS_BLUETOOTH_USAGE:-Bluetooth connects game controllers.}
camera_usage=${RAE_IOS_CAMERA_USAGE:-The camera is used only when you turn on a feature that needs it.}
microphone_usage=${RAE_IOS_MICROPHONE_USAGE:-The microphone is used only when you turn on a feature that needs it.}
for usage in "$bluetooth_usage" "$camera_usage" "$microphone_usage"; do
  case "$usage" in
    *'<'*|*'>'*|*'&'*|*'"'*) echo "error: a purpose string may not contain < > & or \": $usage" >&2; exit 1 ;;
  esac
done
extra_plist="$extra_plist<key>NSBluetoothAlwaysUsageDescription</key><string>$bluetooth_usage</string>"
extra_plist="$extra_plist<key>NSCameraUsageDescription</key><string>$camera_usage</string>"
extra_plist="$extra_plist<key>NSMicrophoneUsageDescription</key><string>$microphone_usage</string>"
# The privacy manifest (PrivacyInfo.xcprivacy): Rae's own by default, which
# declares what the runtime and SDL3 do; RAE_IOS_PRIVACY_MANIFEST replaces it.
privacy_manifest=${RAE_IOS_PRIVACY_MANIFEST:-$here/PrivacyInfo.xcprivacy}
[ -f "$privacy_manifest" ] || { echo "error: no privacy manifest at $privacy_manifest" >&2; exit 1; }
orientations=${RAE_IOS_ORIENTATIONS:-any}
portrait='<string>UIInterfaceOrientationPortrait</string>'
upsideDown='<string>UIInterfaceOrientationPortraitUpsideDown</string>'
landscape='<string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string>'
case "$orientations" in
  portrait) phone_orientations="$portrait"; pad_orientations="$portrait$upsideDown" ;;
  any) phone_orientations="$portrait$upsideDown$landscape"; pad_orientations="$portrait$upsideDown$landscape" ;;
  *) echo "error: RAE_IOS_ORIENTATIONS is portrait or any, not '$orientations'" >&2; exit 1 ;;
esac

rae="$root/compiler/bin/rae"
runtime_dir="$root/compiler/runtime"
frameworks="$here/frameworks"
gen="$root/build/ios/$name"
# Entry: in-repo examples keep main.rae at the dir root; a game project that
# lives in its own repo (e.g. the game) keeps it at src/main.rae. Accept either,
# or an explicit RAE_IOS_ENTRY override.
entry=${RAE_IOS_ENTRY:-}
if [ -z "$entry" ]; then
  if [ -f "$ex_dir/src/main.rae" ]; then entry="$ex_dir/src/main.rae"
  else entry="$ex_dir/main.rae"; fi
fi

[ -x "$rae" ] || { echo "error: build the compiler first ($rae)" >&2; exit 1; }
[ -f "$entry" ] || { echo "error: no main.rae (looked at $ex_dir/src/main.rae and $ex_dir/main.rae)" >&2; exit 1; }
[ -d "$frameworks/SDL3.xcframework" ] || { echo "error: SDL3.xcframework missing — run tools/ios/build-sdl3-ios.sh" >&2; exit 1; }
[ -d "$frameworks/wgpu_native.xcframework" ] || { echo "error: wgpu_native.xcframework missing — run tools/ios/build-wgpu-ios.sh" >&2; exit 1; }

mkdir -p "$gen"

# 1. Emit the app C, then prepend <SDL3/SDL_main.h> so the generated `int main`
#    becomes SDL_main and SDL owns the iOS UIApplication entry.
echo "gen-ios: emitting C for $name..."
"$rae" build --entry "$entry" --project "$ex_dir" --out "$gen/app.c" --target compiled --emit-c --target-os ios --target-arch arm64
{ printf '#include <SDL3/SDL_main.h>\n'; cat "$gen/app.c"; } > "$gen/app_ios.c"

# 2. Enumerate bundled assets (the example's assets/ tree). Each file is added to
#    the bundle at the SAME relative path so the emitted relative reads resolve
#    after the chdir shim.
# Rae examples read assets by two conventions: repo-root-relative (e.g. 136's
# "examples/136_walker_character/assets/...", "lib/data/...") and {dir}/CWD-
# relative ("assets/..."). The chdir shim makes the bundle root the CWD, so we
# STAGE resources under both layouts and bundle them preserving those relative
# paths (mirrors the emcc --preload-file @/path scheme). Shared lib data that
# examples read by repo-root path is staged too.
stage="$gen/bundle"
rm -rf "$stage"; mkdir -p "$stage"
if [ -d "$ex_dir/assets" ]; then
  mkdir -p "$stage/assets";  cp -R "$ex_dir/assets/." "$stage/assets/"  # assets/... (CWD-relative)
  # In-repo examples ALSO read their assets by repo-root path
  # (examples/<name>/assets/...), so stage that layout too. An out-of-tree
  # project (a game with its own repo, read root-relative "assets/..." and
  # "lib/...") is not under $root, where `${ex_dir#"$root/"}` would leave an
  # absolute path and bury the bundle under it — skip the repo-root layer there.
  case "$ex_dir/" in
    "$root"/*)
      ex_rel=${ex_dir#"$root/"}                 # examples/136_walker_character
      mkdir -p "$stage/$ex_rel"; cp -R "$ex_dir/assets" "$stage/$ex_rel/" ;;
  esac
fi
for shared in lib/app3d/scenes lib/data; do
  if [ -e "$root/$shared" ]; then
    dst="$stage/$(dirname "$shared")"; mkdir -p "$dst"; cp -R "$root/$shared" "$dst/"
  fi
done
# Assets an app reads from ANOTHER in-repo example (118/119 draw their debug
# HUD with 136's font): every string the C names under examples/<x>/assets/ is
# staged at its repo-root path. A path built by interpolation is only a prefix
# in the C ("examples/136_walker_character/assets/Roboto-Regular" + ".mtsdf.json"),
# so every file starting with it is copied. On the desktop the CWD is the repo
# root and these resolve on their own; on a device only bundled files exist.
grep -oE '"examples/[A-Za-z0-9_]+/assets/[^"]*"' "$gen/app.c" | tr -d '"' | sort -u \
  | while IFS= read -r ref; do
      for found in "$root/$ref"*; do
        [ -e "$found" ] || continue
        rel=${found#"$root/"}
        mkdir -p "$stage/$(dirname "$rel")"; cp -R "$found" "$stage/$rel"
      done
    done
# Shared WGSL that examples compose at RUNTIME (noise, world_biome, terrain_splat,
# ...). Bundle the whole lib/*.wgsl set: a missing one does not fail gracefully —
# the composed shader loses its entry point and wgpu-native ABORTS at pipeline
# creation (this is what crashed 136 on device: only noise.wgsl was shipped).
# Every lib/**/*.wgsl, at its own relative path (lib/gpu/..., lib/water/...):
# a stdlib read tries the CWD-relative "lib/..." first, and on a device there
# is no $RAE_STDLIB to fall back on.
(cd "$root" && find lib -name '*.wgsl') | while IFS= read -r wgsl; do
  mkdir -p "$stage/$(dirname "$wgsl")"; cp "$root/$wgsl" "$stage/$wgsl"
done

# Box3D (the C-library physics track, lib/box3d): on the desktop `rae` links
# the host's prebuilt libbox3d.a, which is no use on a device, so an app whose
# C includes box3d compiles the pinned Box3D sources (tools/box3d/build.sh's
# clone) and the Rae glue into the iOS target instead.
box3d_sources=""; box3d_includes=""
if grep -q 'box3d/box3d.h' "$gen/app.c"; then
  . "$root/tools/box3d/pin"
  box3d_cache=${RAE_BOX3D_CACHE:-$HOME/.cache/rae/box3d}
  box3d_install=${RAE_BOX3D:-$box3d_cache/install}
  box3d_src="$box3d_cache/src"
  if [ ! -d "$box3d_src/src" ] || [ ! -f "$box3d_install/include/box3d/rae_glue.h" ]; then
    echo "error: this app imports box3d; run tools/box3d/build.sh first (clones the pinned Box3D)" >&2; exit 1
  fi
  if [ "$(git -C "$box3d_src" rev-parse --short=7 HEAD)" != "$BOX3D_COMMIT" ]; then
    echo "error: $box3d_src is not at the Box3D pin $BOX3D_COMMIT; run tools/box3d/build.sh" >&2; exit 1
  fi
  echo "gen-ios: compiling Box3D $BOX3D_COMMIT from source into the app"
  for file in "$box3d_src"/src/*.c "$root/tools/box3d/rae_glue.c"; do
    box3d_sources="${box3d_sources}  \"$file\"\n"
  done
  box3d_includes="  \"$box3d_install/include\"\n  \"$box3d_src/src\"\n"
fi

asset_lines=""; pkg_lines=""
while IFS= read -r f; do
  [ -n "$f" ] || continue
  rel=${f#"$stage/"}; reldir=$(dirname "$rel")
  asset_lines="${asset_lines}  \"$f\"\n"
  pkg_lines="${pkg_lines}set_source_files_properties(\"$f\" PROPERTIES MACOSX_PACKAGE_LOCATION \"$reldir\")\n"
done <<EOF
$(find "$stage" -type f)
EOF

# The icon's asset catalog, compiled by Xcode's actool into the bundle (a
# RESOURCE of the target), named by ASSETCATALOG_COMPILER_APPICON_NAME.
# The privacy manifest goes to the bundle's root.
mkdir -p "$gen/privacy"
cp "$privacy_manifest" "$gen/privacy/PrivacyInfo.xcprivacy"
resources="$gen/privacy/PrivacyInfo.xcprivacy"
icon_source=""; icon_properties=""
if [ -n "$icon" ]; then
  catalog="$gen/Assets.xcassets"
  rm -rf "$catalog"; mkdir -p "$catalog/AppIcon.appiconset"
  cp "$icon" "$catalog/AppIcon.appiconset/AppIcon-1024.png"
  printf '{ "info": { "author": "rae", "version": 1 } }\n' > "$catalog/Contents.json"
  cat > "$catalog/AppIcon.appiconset/Contents.json" <<EOF
{
  "images": [
    { "filename": "AppIcon-1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024" }
  ],
  "info": { "author": "rae", "version": 1 }
}
EOF
  icon_source="  \"$catalog\"\n"
  resources="$resources;$catalog"
  icon_properties="set_target_properties(\${APP} PROPERTIES
  XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME \"AppIcon\")\n"
fi

# 3. Emit CMakeLists.
cat > "$gen/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.28)
project(RaeApp C)

set(APP RaeApp)
set(RAE_IOS_PHONE_ORIENTATIONS "$phone_orientations")
set(RAE_IOS_PAD_ORIENTATIONS "$pad_orientations")
set(RAE_IOS_EXTRA_PLIST "$extra_plist")
add_executable(\${APP} MACOSX_BUNDLE
  "$gen/app_ios.c"
  "$runtime_dir/rae_runtime.c"
  "$here/ios_bootstrap.c"
  "$gen/privacy/PrivacyInfo.xcprivacy"
$(printf "%b" "$box3d_sources")$(printf "%b" "$icon_source")$(printf "%b" "$asset_lines"))

$(printf "%b" "${pkg_lines:-}")

target_compile_definitions(\${APP} PRIVATE RAE_HAS_SDL3 RAE_HAS_WEBGPU)
target_compile_options(\${APP} PRIVATE -w -ffp-contract=off -g)
target_include_directories(\${APP} PRIVATE
  "$runtime_dir"
  "$frameworks/SDL3.xcframework/ios-arm64/Headers"
  "$frameworks/SDL3.xcframework/ios-arm64-simulator/Headers"
  "$frameworks/wgpu_native.xcframework/ios-arm64/Headers"
  "$frameworks/wgpu_native.xcframework/ios-arm64-simulator/Headers"
$(printf "%b" "$box3d_includes"))

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

# A real Info.plist (with UILaunchScreen for native resolution + portrait lock);
# CMake's INFOPLIST_KEY_* build settings do not apply to a MACOSX_BUNDLE target.
set_target_properties(\${APP} PROPERTIES
  MACOSX_BUNDLE_GUI_IDENTIFIER "$bundle_id"
  MACOSX_BUNDLE_BUNDLE_NAME "$disp"
  MACOSX_BUNDLE_INFO_PLIST "$here/Info.plist.in"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "$version"
  MACOSX_BUNDLE_BUNDLE_VERSION "$build_number"
  RESOURCE "$resources"
  XCODE_ATTRIBUTE_MARKETING_VERSION "$version"
  XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION "$build_number"
  # An archive keeps the app only if it is installed into Applications
  XCODE_ATTRIBUTE_SKIP_INSTALL "NO"
  # Debug symbols beside the app, so crash reports (TestFlight's too) name
  # the functions
  XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym"
  XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS "YES"
  XCODE_ATTRIBUTE_INSTALL_PATH "\$(LOCAL_APPS_DIR)"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "$bundle_id"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
  XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
  XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "$deployment_target")
$(printf "%b" "$icon_properties")
EOF

# 4. Configure the Xcode project.
echo "gen-ios: configuring Xcode project..."
cmake -S "$gen" -B "$gen/proj" -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" >/dev/null

# 5. Metadata for run-ios.sh (bundle id, project, signed .app path).
cat > "$gen/meta.env" <<EOF
RAE_IOS_NAME="$name"
RAE_IOS_BUNDLE_ID="$bundle_id"
RAE_IOS_PROJ="$gen/proj/RaeApp.xcodeproj"
RAE_IOS_APP="$gen/proj/Release-iphoneos/RaeApp.app"
RAE_IOS_ARCHIVE="$gen/RaeApp.xcarchive"
EOF

echo ""
echo "gen-ios: project at $gen/proj/RaeApp.xcodeproj (meta: $gen/meta.env)"
echo "  headless run: RAE_IOS_TEAM=<team> sh tools/ios/run-ios.sh $name"
echo "  or GUI: open '$gen/proj/RaeApp.xcodeproj'"
