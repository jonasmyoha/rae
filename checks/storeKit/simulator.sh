#!/bin/bash
# The storeKit check (Main.rae beside this) as an iOS app in the iOS Simulator (docs/store-kit.md):
#
#   bash checks/storeKit/simulator.sh
#
# tools/ios/gen-ios.sh writes the app's Xcode project for the simulator (RAE_IOS_SDK), with the
# package's Swift glue compiled into the app; xcodebuild builds it unsigned; the script boots an
# iPhone simulator if none is booted (RAE_SIMULATOR_DEVICE: a device id or name), installs and
# launches the app, and reads the app's log (the iOS launch shim writes stdout to
# Documents/rae.log) until the check's verdict, for up to 60 s. Then it ends the app, uninstalls
# it and shuts down a simulator it booted. Exit code 0 = the check passed.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
bundle="com.example.rae.storekitcheck"
work="$(mktemp -d -t rae_storekit_simulator_XXXXXX)"
device=""
booted_here=0
cleanup() {
  if [ -n "$device" ]; then
    xcrun simctl terminate "$device" "$bundle" >/dev/null 2>&1 || true
    xcrun simctl uninstall "$device" "$bundle" >/dev/null 2>&1 || true
    if [ "$booted_here" = 1 ]; then
      xcrun simctl shutdown "$device" >/dev/null 2>&1 || true
    fi
  fi
  rm -rf "$work"
}
trap cleanup EXIT

if [ "$(uname -s)" != "Darwin" ] || ! command -v xcodebuild >/dev/null 2>&1; then
  echo "storeKit simulator: needs a Mac with Xcode; nothing to test here"
  exit 0
fi

# 1. The app, for the simulator
export RAE_TOOLCHAIN_CHECK=off RAE_IOS_SDK=iphonesimulator RAE_IOS_BUILD_NAME=storeKitSimulator
sh "$root/tools/ios/gen-ios.sh" "$here" "$bundle" "StoreKit check" > "$work/gen.log" 2>&1 \
  || { cat "$work/gen.log"; exit 1; }
project="$root/build/ios/storeKitSimulator/proj"
xcodebuild -project "$project/RaeApp.xcodeproj" -scheme RaeApp -configuration Debug -sdk iphonesimulator \
  -destination 'generic/platform=iOS Simulator' -derivedDataPath "$work/derived" CODE_SIGNING_ALLOWED=NO build \
  > "$work/build.log" 2>&1 || { grep -E 'error|^ld:' "$work/build.log" | head -20; exit 1; }
app="$(find "$project" -name RaeApp.app -path '*-iphonesimulator/*' | head -1)"
[ -n "$app" ] || { echo "storeKit simulator: no RaeApp.app was built" >&2; exit 1; }

# 2. A simulator: the one asked for, else a booted iPhone, else the first available one booted here
pick() { xcrun simctl list devices available | grep -E "$1" | head -1 | sed -E 's/.*\(([0-9A-F-]{36})\).*/\1/'; }
if [ -n "${RAE_SIMULATOR_DEVICE:-}" ]; then
  device="$(pick "$RAE_SIMULATOR_DEVICE")"
else
  device="$(pick 'iPhone.*\(Booted\)')"
  [ -n "$device" ] || device="$(pick 'iPhone')"
fi
[ -n "$device" ] || { echo "storeKit simulator: no iPhone simulator is available" >&2; exit 1; }
if ! xcrun simctl list devices | grep "$device" | grep -q Booted; then
  xcrun simctl boot "$device"
  booted_here=1
fi
xcrun simctl bootstatus "$device" -b >/dev/null

# 3. Install, launch, read the verdict
xcrun simctl install "$device" "$app"
xcrun simctl launch "$device" "$bundle" >/dev/null
log="$(xcrun simctl get_app_container "$device" "$bundle" data)/Documents/rae.log"
for _ in $(seq 1 60); do
  if [ -f "$log" ] && grep -qE 'STOREKIT OK|FAIL' "$log"; then break; fi
  sleep 1
done
[ -f "$log" ] && grep -v '^@@' "$log"
if [ -f "$log" ] && grep -q 'STOREKIT OK' "$log" && ! grep -q 'FAIL' "$log"; then
  echo "storeKit simulator: PASSED"
  exit 0
fi
echo "storeKit simulator: FAILED"
exit 1
