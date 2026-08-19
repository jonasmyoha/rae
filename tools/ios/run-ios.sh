#!/bin/sh
# Build, sign, install, and launch a Rae example on a connected iOS device —
# fully headless, no Xcode GUI (#520). Uses xcodebuild (automatic provisioning)
# + xcrun devicectl. Xcode must be installed and your Apple ID signed into
# Xcode → Settings → Accounts (one-time); after that this is scriptable.
#
#   RAE_IOS_TEAM=<team-id> sh tools/ios/run-ios.sh <example-dir-or-name> [device]
#
# RAE_IOS_TEAM   your Apple Developer team id (required; e.g. from an installed
#                provisioning profile's TeamIdentifier).
# device / RAE_IOS_DEVICE  a devicectl device identifier or name; default =
#                the first "available" device.
# RAE_IOS_PROFILE  seconds to capture a frame profile (#527). When set, the app
#                launches with RAE_PROFILE on, you play for that long, and the
#                Chrome-trace JSON is pulled off the device to RAE_PROFILE_LOCAL
#                (default /tmp/rae_profile.json) — open it at ui.perfetto.dev.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
ex=${1:?usage: RAE_IOS_TEAM=<id> run-ios.sh <example> [device]}
name=$(basename "$ex")

# Team: RAE_IOS_TEAM, else inferred from the first installed provisioning profile
# (its TeamIdentifier) so the devtools "iOS device" button works with no env.
team=${RAE_IOS_TEAM:-}
if [ -z "$team" ]; then
  prof=$(find "$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles" \
              "$HOME/Library/MobileDevice/Provisioning Profiles" \
              -name '*.mobileprovision' 2>/dev/null | head -1)
  if [ -n "$prof" ]; then
    team=$(security cms -D -i "$prof" 2>/dev/null | plutil -extract TeamIdentifier.0 raw - 2>/dev/null || true)
  fi
fi
[ -n "$team" ] || { echo "error: set RAE_IOS_TEAM (Apple Developer team id) — none inferable from provisioning profiles" >&2; exit 1; }

# 1. Generate/refresh the Xcode project (idempotent).
sh "$here/gen-ios.sh" "$ex" >/dev/null
. "$root/build/ios/$name/meta.env"

# 2. Pick the device: arg > RAE_IOS_DEVICE > first "available" from devicectl.
device=${2:-${RAE_IOS_DEVICE:-}}
if [ -z "$device" ]; then
  device=$(xcrun devicectl list devices 2>/dev/null \
    | awk -F'  +' '/available/ && !/unavailable/ {print $3; exit}')
fi
[ -n "$device" ] || { echo "error: no available iOS device (plug one in / trust it)" >&2; exit 1; }
echo "run-ios: $name → device $device (team $team)"

# 3. Build + sign (automatic provisioning).
echo "run-ios: building + signing..."
xcodebuild -project "$RAE_IOS_PROJ" -scheme RaeApp -configuration Release \
  -sdk iphoneos -destination 'generic/platform=iOS' \
  -allowProvisioningUpdates DEVELOPMENT_TEAM="$team" build >/dev/null

# 4. Install + launch.
echo "run-ios: installing..."
xcrun devicectl device install app --device "$device" "$RAE_IOS_APP" >/dev/null

# 5. Launch — optionally with a frame-profile capture (#527).
prof_secs=${RAE_IOS_PROFILE:-}
if [ -n "$prof_secs" ]; then
  echo "run-ios: launching $RAE_IOS_BUNDLE_ID with a ${prof_secs}s profile capture..."
  # devicectl takes env as a JSON dict; the bootstrap points RAE_PROFILE_OUT at
  # the writable Documents dir (see ios_bootstrap.c).
  xcrun devicectl device process launch --device "$device" \
    -e "{\"RAE_PROFILE\":\"1\",\"RAE_PROFILE_SECONDS\":\"$prof_secs\"}" \
    "$RAE_IOS_BUNDLE_ID"
  echo "run-ios: PLAY NOW — walk around / let the sun set for ~${prof_secs}s..."
  sleep $((prof_secs + 6))
  local_out=${RAE_PROFILE_LOCAL:-/tmp/rae_profile.json}
  echo "run-ios: pulling trace to $local_out..."
  xcrun devicectl device copy from --device "$device" \
    --domain-type appDataContainer --domain-identifier "$RAE_IOS_BUNDLE_ID" \
    --source Documents/rae_profile.json --destination "$local_out"
  echo "run-ios: profile written to $local_out — open at https://ui.perfetto.dev"
else
  echo "run-ios: launching $RAE_IOS_BUNDLE_ID..."
  xcrun devicectl device process launch --device "$device" "$RAE_IOS_BUNDLE_ID"
  echo "run-ios: launched $name on device."
fi
