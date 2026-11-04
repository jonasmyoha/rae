#!/bin/sh
# Generate an iOS Xcode project for ONE Rae example (#520; seed of the multi-target
# generator #521). Reuses the compiler's `--emit-c`, links the two iOS xcframeworks
# (build them first: build-wgpu-ios.sh, build-sdl3-ios.sh), bundles the example's
# assets preserving relative paths, and produces build/ios/<name>/RaeApp.xcodeproj
# (<name>: RAE_IOS_BUILD_NAME, default the project folder's name).
#
#   sh tools/ios/gen-ios.sh <project-dir> [bundle-id] [display-name]
#   e.g. sh tools/ios/gen-ios.sh examples/102_gpu2d_animated
#
# <project-dir> is an in-repo example (entry main.rae, assets read by repo-root
# path too) OR an out-of-tree game repo (entry src/main.rae, assets/ and lib/*
# read CWD-relative). Both are handled; RAE_IOS_ENTRY overrides the entry file.
#
# Then: open build/ios/<name>/proj/RaeApp.xcodeproj, pick your team (Signing &
# Capabilities → Automatic), select the device, and Run. (Signing needs you —
# it can't be scripted headlessly.)
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
# In the store, the shared builds of this version (tools/toolchain-prepare.sh)
[ ! -f "$root/.deps" ] || . "$root/.deps"
ex_arg=${1:?usage: gen-ios.sh <example-dir> [bundle-id] [name]}
if [ -d "$root/$ex_arg" ]; then
  ex_dir=$(CDPATH= cd -- "$root/$ex_arg" && pwd)
elif [ -d "$ex_arg" ]; then
  ex_dir=$(CDPATH= cd -- "$ex_arg" && pwd)
else
  echo "error: example dir not found: $ex_arg" >&2; exit 1
fi
name=$(basename "$ex_dir")
# The build folder's name, build/ios/<name> (RAE_IOS_BUILD_NAME, default the
# project folder's name): two projects whose folders share a generic name
# (`shell`, `app`) set it so they do not overwrite each other's Xcode project.
build_name=${RAE_IOS_BUILD_NAME:-$name}
case "$build_name" in
  ""|*/*|.|..) echo "error: RAE_IOS_BUILD_NAME must be one folder name, not '$build_name'" >&2; exit 1 ;;
esac
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
# The local network (an address like 192.168.1.20 or a name like
# mymac.local): iOS refuses such a connection, without asking the person,
# unless Info.plist says why the app needs it. Written only when
# RAE_IOS_LOCAL_NETWORK_USAGE is set, so other apps are unchanged.
# RAE_IOS_BONJOUR_SERVICES lists the Bonjour service types the app browses or
# publishes, space-separated ("_sumu._tcp"); browsing asks the same question,
# so it needs the text too.
local_network_usage=${RAE_IOS_LOCAL_NETWORK_USAGE:-}
bonjour_services=${RAE_IOS_BONJOUR_SERVICES:-}
if [ -n "$local_network_usage" ]; then
  case "$local_network_usage" in
    *'<'*|*'>'*|*'&'*|*'"'*) echo "error: a purpose string may not contain < > & or \": $local_network_usage" >&2; exit 1 ;;
  esac
  extra_plist="$extra_plist<key>NSLocalNetworkUsageDescription</key><string>$local_network_usage</string>"
fi
if [ -n "$bonjour_services" ]; then
  if [ -z "$local_network_usage" ]; then
    echo "error: RAE_IOS_BONJOUR_SERVICES needs RAE_IOS_LOCAL_NETWORK_USAGE (iOS asks the person with that text)" >&2
    exit 1
  fi
  services=""
  for service in $bonjour_services; do
    case "$service" in
      _*._tcp|_*._udp) ;;
      *) echo "error: a Bonjour service type is written _name._tcp or _name._udp, not: $service" >&2; exit 1 ;;
    esac
    case "$service" in
      *[!A-Za-z0-9._-]*) echo "error: a Bonjour service type may hold only letters, digits, - and _: $service" >&2; exit 1 ;;
    esac
    services="$services<string>$service</string>"
  done
  extra_plist="$extra_plist<key>NSBonjourServices</key><array>$services</array>"
fi
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
gen="$root/build/ios/$build_name"
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

# Where the device is (lib/Location): iOS asks the person for it only with a
# purpose string in Info.plist (NSLocationWhenInUseUsageDescription), and App
# Store Connect refuses a build that uses Core Location without one. An app
# whose C includes the Location shim links CoreLocation and must set
# RAE_IOS_LOCATION_USAGE (plain text, no < & >: why it needs the location);
# the text is written whenever it is set.
location_usage=${RAE_IOS_LOCATION_USAGE:-}
location_link=""
if grep -q 'runtime_location_apple.h' "$gen/app.c"; then
  if [ -z "$location_usage" ]; then
    echo "error: the app uses lib/Location: set RAE_IOS_LOCATION_USAGE to the words iOS shows when it asks (why the app needs the location)" >&2
    exit 1
  fi
  location_link=' "-framework CoreLocation"'
fi
if [ -n "$location_usage" ]; then
  case "$location_usage" in
    *'<'*|*'>'*|*'&'*|*'"'*) echo "error: a purpose string may not contain < > & or \": $location_usage" >&2; exit 1 ;;
  esac
  extra_plist="$extra_plist<key>NSLocationWhenInUseUsageDescription</key><string>$location_usage</string>"
fi

# Notifications at a later time (lib/LocalNotification): an app whose C includes the shim
# links UserNotifications. iOS needs no purpose string for them.
notification_link=""
if grep -q 'runtime_local_notification_apple.h' "$gen/app.c"; then
  notification_link=' "-framework UserNotifications"'
fi

# The links that open this app (docs/app-links.md). RAE_APP_LINKS lists hosts
# (host or host/path; iOS takes the paths from the website's
# apple-app-site-association, so only the host is used here): each becomes an
# applinks: associated domain in an entitlements file, a capability the team's
# App ID must allow. RAE_APP_SCHEME registers a custom scheme in Info.plist.
entitlements_line=""
if [ -n "${RAE_APP_LINKS:-}" ]; then
  domains=""
  for link in $RAE_APP_LINKS; do
    case "$link" in
      *'<'*|*'>'*|*'&'*|*'"'*|*://*) echo "error: an RAE_APP_LINKS entry is host or host/path, no scheme: $link" >&2; exit 1 ;;
    esac
    domains="$domains<string>applinks:${link%%/*}</string>"
  done
  cat > "$gen/RaeApp.entitlements" <<ENTITLEMENTS
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.developer.associated-domains</key><array>$domains</array></dict></plist>
ENTITLEMENTS
  entitlements_line="XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS \"$gen/RaeApp.entitlements\""
fi
if [ -n "${RAE_APP_SCHEME:-}" ]; then
  case "$RAE_APP_SCHEME" in
    *[!a-z0-9.+-]*) echo "error: RAE_APP_SCHEME is lowercase letters, digits, . + -: $RAE_APP_SCHEME" >&2; exit 1 ;;
  esac
  extra_plist="$extra_plist<key>CFBundleURLTypes</key><array><dict><key>CFBundleURLName</key><string>$bundle_id</string><key>CFBundleURLSchemes</key><array><string>$RAE_APP_SCHEME</string></array></dict></array>"
fi

# 2. Stage the files the app reads at run time (tools/stage-app-assets.sh,
#    shared with tools/android): each at the relative path the app names it
#    by, so the reads resolve after the chdir shim.
stage="$gen/bundle"
sh "$root/tools/stage-app-assets.sh" "$ex_dir" "$gen/app.c" "$stage"

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

wamr_install=""
if grep -q 'runtime_wasm_embed.h' "$gen/app.c"; then
  wamr_install=${RAE_WAMR:-${RAE_WAMR_CACHE:-$HOME/.cache/rae/wamr}/install}
  if [ ! -f "$wamr_install/lib/ios/librae_wamr.a" ]; then
    echo "error: this app imports wasm; run tools/wamr/build.sh first (builds WAMR for iOS)" >&2; exit 1
  fi
  echo "gen-ios: linking WAMR $(cat "$wamr_install/COMMIT") (fast interpreter) into the app"
fi
# WAMR (lib/wasm): an app whose C includes the glue header links the iOS
# builds of the fast interpreter tools/wamr/build.sh installed (interpreter
# only: iOS allows no JIT), the device's or the simulator's by SDK.
wamr_cmake=""
if [ -n "$wamr_install" ]; then
  wamr_cmake="set_property(TARGET \${APP} PROPERTY XCODE_ATTRIBUTE_LIBRARY_SEARCH_PATHS[sdk=iphoneos*] \"$wamr_install/lib/ios\")\n"
  wamr_cmake="${wamr_cmake}set_property(TARGET \${APP} PROPERTY XCODE_ATTRIBUTE_LIBRARY_SEARCH_PATHS[sdk=iphonesimulator*] \"$wamr_install/lib/ios-simulator\")\n"
  wamr_cmake="${wamr_cmake}target_link_libraries(\${APP} \"-lrae_wamr\")\n"
fi

# The packages with C (a `native` block, docs/toolchain-and-packages-decision.md): an app whose C
# includes a package's header compiles its sources into the app, with its options.
package_definitions=""
package_lines=$(cd "$ex_dir" && "$rae" tree --native 2>/dev/null || true)
if [ -n "$package_lines" ]; then
  tab=$(printf '\t')
  while IFS="$tab" read -r package_name package_header package_install package_sources package_defines; do
    grep -q "$package_header" "$gen/app.c" || continue
    if [ -z "$package_sources" ]; then
      echo "error: package $package_name has no sourcesDevice, so it cannot build for a device" >&2
      exit 1
    fi
    echo "gen-ios: compiling package $package_name from source into the app"
    for package_source in $package_sources; do
      box3d_sources="${box3d_sources}  \"$package_install/$package_source\"\n"
    done
    box3d_includes="${box3d_includes}  \"$package_install/include\"\n"
    if [ -n "$package_defines" ]; then
      package_definitions="$package_definitions $(sed 's/-D//g' "$package_install/$package_defines")"
    fi
  done <<EOF
$package_lines
EOF
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

target_compile_definitions(\${APP} PRIVATE RAE_HAS_SDL3 RAE_HAS_WEBGPU $package_definitions)
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
  "-framework ImageIO" "-framework CoreMotion" "-framework GameController" "-framework Network"
  "-framework AVFoundation" "-framework AudioToolbox" "-framework CoreHaptics"
  "-framework CoreText" "-framework UniformTypeIdentifiers"
  "-framework CoreBluetooth" "-framework CoreMedia" "-framework CoreVideo"
  "-framework OpenGLES" "-framework Security"$location_link$notification_link)

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
  $entitlements_line
  XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "$deployment_target")
$(printf "%b" "$icon_properties")
$(printf "%b" "$wamr_cmake")
EOF

# 4. Configure the Xcode project.
echo "gen-ios: configuring Xcode project..."
cmake -S "$gen" -B "$gen/proj" -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" >/dev/null

# The .app an earlier build assembled. The staging folder above starts empty
# and the project lists exactly its files, but Xcode's copy of the resources
# into an existing .app only adds and updates: a file deleted from the
# project's assets since then stayed in every later build. Removing the
# bundle makes the next build assemble it from the current list (the object
# files stay, so it only copies, links and signs again). A link left by
# upload-ios.sh's archive is removed as a link.
for built_app in "$gen"/proj/*/RaeApp.app; do
  [ -e "$built_app" ] || [ -L "$built_app" ] || continue
  rm -rf "$built_app"
done

# 5. Metadata for run-ios.sh (bundle id, project, signed .app path).
cat > "$gen/meta.env" <<EOF
RAE_IOS_NAME="$name"
RAE_IOS_BUILD_NAME="$build_name"
RAE_IOS_BUNDLE_ID="$bundle_id"
RAE_IOS_PROJ="$gen/proj/RaeApp.xcodeproj"
RAE_IOS_APP="$gen/proj/Release-iphoneos/RaeApp.app"
RAE_IOS_ARCHIVE="$gen/RaeApp.xcarchive"
EOF

echo ""
echo "gen-ios: project at $gen/proj/RaeApp.xcodeproj (meta: $gen/meta.env)"
run_env=""
[ "$build_name" = "$name" ] || run_env="RAE_IOS_BUILD_NAME=$build_name "
echo "  headless run: ${run_env}RAE_IOS_TEAM=<team> sh tools/ios/run-ios.sh $ex_dir"
echo "  or GUI: open '$gen/proj/RaeApp.xcodeproj'"
