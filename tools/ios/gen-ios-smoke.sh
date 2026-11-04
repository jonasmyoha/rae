#!/bin/sh
# Smoke test of gen-ios.sh's bundle: what the app ships is exactly the
# project's assets now, never a file deleted since an earlier build.
#
#   sh tools/ios/gen-ios-smoke.sh
#
# It generates a throwaway project twice (no xcodebuild, no device, no
# signing): between the runs one asset is deleted from the project and an
# earlier build's .app holding it is faked. Checked after each run:
#   - the staging folder holds the project's assets and the stdlib's (the
#     Material icon atlas, a lib/assets picture), and nothing else of the
#     project's;
#   - the generated project lists the deleted asset no more;
#   - the earlier build's .app is gone, so the next build assembles the
#     bundle from the current list;
#   - Info.plist gets NSLocalNetworkUsageDescription and NSBonjourServices
#     only when RAE_IOS_LOCAL_NETWORK_USAGE / RAE_IOS_BONJOUR_SERVICES ask,
#     and Bonjour without the text, or a malformed service type, is refused.
#   - Info.plist gets NSLocationWhenInUseUsageDescription only when
#     RAE_IOS_LOCATION_USAGE asks, and an app without lib/Location does not
#     link CoreLocation.
# Needs the compiler and the two iOS xcframeworks, like gen-ios.sh.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
build_name=genIosSmoke
gen="$root/build/ios/$build_name"
project=$(mktemp -d "${TMPDIR:-/tmp}/rae-gen-ios-smoke.XXXXXX")
trap 'rm -rf "$project" "$gen"' EXIT

mkdir -p "$project/assets/scenes"
cat > "$project/Main.rae" <<'EOF'
func main() {
  log("gen-ios smoke")
}
EOF
printf 'kept\n' > "$project/assets/scenes/Kept.raescene"
printf 'deleted\n' > "$project/assets/scenes/Deleted.raescene"

failures=0
check() {
  if eval "$2"; then
    echo "PASS: $1"
  else
    echo "FAIL: $1"
    failures=$((failures + 1))
  fi
}

generate() {
  RAE_IOS_BUILD_NAME=$build_name sh "$here/gen-ios.sh" "$project" > "$gen.log" 2>&1 \
    || { cat "$gen.log" >&2; echo "FAIL: gen-ios.sh exited non-zero" >&2; exit 1; }
}

mkdir -p "$(dirname "$gen")"
generate
check "first run stages both assets" \
  '[ -f "$gen/bundle/assets/scenes/Kept.raescene" ] && [ -f "$gen/bundle/assets/scenes/Deleted.raescene" ]'
check "the stdlib's icon atlas is staged" \
  '[ -f "$gen/bundle/lib/assets/fonts/MaterialIconsRound-Regular.mtsdf.json" ] && [ -f "$gen/bundle/lib/assets/fonts/MaterialIconsRound-Regular.mtsdf.png" ]'
check "a lib/assets picture is staged" '[ -f "$gen/bundle/lib/assets/grass/meadowTuft.png" ]'

# an earlier build's .app that still holds the asset, then the asset deleted
app="$gen/proj/Release-iphoneos/RaeApp.app"
mkdir -p "$app/assets/scenes"
printf 'deleted\n' > "$app/assets/scenes/Deleted.raescene"
rm "$project/assets/scenes/Deleted.raescene"
generate
check "the deleted asset is not staged" '[ ! -e "$gen/bundle/assets/scenes/Deleted.raescene" ]'
check "the kept asset is still staged" '[ -f "$gen/bundle/assets/scenes/Kept.raescene" ]'
check "the project no longer lists the deleted asset" '! grep -q "Deleted.raescene" "$gen/CMakeLists.txt"'
check "the earlier build's .app is removed" '[ ! -e "$app" ]'
check "the staging holds no other project file" \
  '[ "$(find "$gen/bundle/assets" -type f | wc -l | tr -d " ")" = "1" ]'
check "no local network key unless asked" '! grep -q "NSLocalNetworkUsageDescription" "$gen/CMakeLists.txt"'
check "no Bonjour key unless asked" '! grep -q "NSBonjourServices" "$gen/CMakeLists.txt"'
check "no location key unless asked" '! grep -q "NSLocationWhenInUseUsageDescription" "$gen/CMakeLists.txt"'
check "CoreLocation is not linked by an app without lib/Location" '! grep -q "CoreLocation" "$gen/CMakeLists.txt"'
check "UserNotifications is not linked by an app without lib/LocalNotification" '! grep -q "UserNotifications" "$gen/CMakeLists.txt"'

# the local network permission, asked for
export RAE_IOS_LOCAL_NETWORK_USAGE="Sync talks to your Mac on this network."
export RAE_IOS_BONJOUR_SERVICES="_sumu._tcp _sumu._udp"
export RAE_IOS_LOCATION_USAGE="The weather is shown for where you are."
generate
unset RAE_IOS_LOCAL_NETWORK_USAGE RAE_IOS_BONJOUR_SERVICES RAE_IOS_LOCATION_USAGE
check "the location text is in Info.plist" \
  'grep -q "<key>NSLocationWhenInUseUsageDescription</key><string>The weather is shown for where you are.</string>" "$gen/CMakeLists.txt"'

check "the local network text is in Info.plist" \
  'grep -q "<key>NSLocalNetworkUsageDescription</key><string>Sync talks to your Mac on this network.</string>" "$gen/CMakeLists.txt"'
check "the Bonjour services are in Info.plist" \
  'grep -q "<key>NSBonjourServices</key><array><string>_sumu._tcp</string><string>_sumu._udp</string></array>" "$gen/CMakeLists.txt"'
check "Bonjour services without the text are refused" \
  '! RAE_IOS_BUILD_NAME=$build_name RAE_IOS_BONJOUR_SERVICES="_sumu._tcp" sh "$here/gen-ios.sh" "$project" > "$gen.log" 2>&1'
check "a malformed Bonjour type is refused" \
  '! RAE_IOS_BUILD_NAME=$build_name RAE_IOS_LOCAL_NETWORK_USAGE="x" RAE_IOS_BONJOUR_SERVICES="sumu" sh "$here/gen-ios.sh" "$project" > "$gen.log" 2>&1'
rm -f "$gen.log"

if [ "$failures" -gt 0 ]; then
  echo "gen-ios-smoke: $failures check(s) failed"
  exit 1
fi
echo "gen-ios-smoke: all checks passed"
