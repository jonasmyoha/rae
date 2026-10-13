#!/bin/sh
# Build a Rae project for the App Store and upload it to App Store Connect,
# where it appears in TestFlight (internal testers get it once Apple has
# processed it, usually 5-30 minutes; external testers need a beta review).
# Headless: xcodebuild archives with automatic signing (making the Apple
# Distribution certificate and the App Store profile if the account may), then
# exports straight to App Store Connect.
#
#   RAE_IOS_TEAM=<team-id> RAE_IOS_BUNDLE_ID=<id> RAE_IOS_VERSION=<x.y.z> \
#     RAE_IOS_BUILD=<n> sh tools/ios/upload-ios.sh <project-dir>
#
# Before the first upload: an explicit App ID for the bundle id in the
# developer account, and an app with that bundle id in App Store Connect.
# RAE_IOS_BUILD must be higher than every earlier upload of the same version.
# The gen-ios.sh options (RAE_IOS_NAME, RAE_IOS_ICON, RAE_IOS_ORIENTATIONS,
# RAE_IOS_EXEMPT_ENCRYPTION, RAE_IOS_PRIVACY_MANIFEST, ...) apply.
#
# Who uploads: the Apple ID signed into Xcode -> Settings -> Accounts, or an
# App Store Connect API key (Users and Access -> Integrations), given as
#   RAE_IOS_ASC_KEY_PATH=<AuthKey_XXXX.p8> RAE_IOS_ASC_KEY_ID=<key id>
#   RAE_IOS_ASC_ISSUER_ID=<issuer id>
# RAE_IOS_UPLOAD=0 stops after the export (an .ipa in the build dir, nothing sent).
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
ex=${1:?usage: RAE_IOS_TEAM=<id> RAE_IOS_BUILD=<n> upload-ios.sh <project-dir>}
name=$(basename "$ex")
team=${RAE_IOS_TEAM:?set RAE_IOS_TEAM (the Apple Developer team id)}
[ -n "${RAE_IOS_BUILD:-}" ] || { echo "error: set RAE_IOS_BUILD (higher than every earlier upload of this version)" >&2; exit 1; }

# 1. Generate the Xcode project with this version and build number.
sh "$here/gen-ios.sh" "$ex" >/dev/null
. "$root/build/ios/$name/meta.env"
gen="$root/build/ios/$name"

auth=""
if [ -n "${RAE_IOS_ASC_KEY_PATH:-}" ]; then
  auth="-authenticationKeyPath $RAE_IOS_ASC_KEY_PATH -authenticationKeyID ${RAE_IOS_ASC_KEY_ID:?set RAE_IOS_ASC_KEY_ID} -authenticationKeyIssuerID ${RAE_IOS_ASC_ISSUER_ID:?set RAE_IOS_ASC_ISSUER_ID}"
fi

# 2. Archive (Release, device, automatic signing).
echo "upload-ios: archiving $name ${RAE_IOS_VERSION:-1.0} (${RAE_IOS_BUILD}) for team $team..."
rm -rf "$RAE_IOS_ARCHIVE"
# shellcheck disable=SC2086
xcodebuild -project "$RAE_IOS_PROJ" -scheme RaeApp -configuration Release \
  -sdk iphoneos -destination 'generic/platform=iOS' \
  -archivePath "$RAE_IOS_ARCHIVE" -allowProvisioningUpdates $auth \
  DEVELOPMENT_TEAM="$team" CODE_SIGN_STYLE=Automatic archive > "$gen/archive.log" 2>&1 \
  || { tail -30 "$gen/archive.log" >&2; echo "error: archive failed (full log: $gen/archive.log)" >&2; exit 1; }
[ -d "$RAE_IOS_ARCHIVE/Products/Applications" ] \
  || { echo "error: the archive has no app in Products/Applications (a generic archive)" >&2; exit 1; }

# The debug symbols: Xcode writes the dSYM where CMake's project builds
# (CONFIGURATION_BUILD_DIR), outside the archive, so it is copied into the
# archive's dSYMs if it belongs to the archived binary (the same UUIDs) - for
# crash reports, TestFlight's too, that name the functions.
archived="$RAE_IOS_ARCHIVE/Products/Applications/RaeApp.app/RaeApp"
built_dsym="$(dirname "$RAE_IOS_APP")/RaeApp.app.dSYM"
if [ -z "$(ls -A "$RAE_IOS_ARCHIVE/dSYMs" 2>/dev/null)" ] && [ -d "$built_dsym" ]; then
  if [ "$(dwarfdump --uuid "$archived" | cut -d' ' -f2 | sort)" = "$(dwarfdump --uuid "$built_dsym" | cut -d' ' -f2 | sort)" ]; then
    mkdir -p "$RAE_IOS_ARCHIVE/dSYMs"
    cp -R "$built_dsym" "$RAE_IOS_ARCHIVE/dSYMs/"
  else
    echo "upload-ios: warning: $built_dsym is not the archived binary's; uploading without symbols" >&2
  fi
fi

# 3. Export for App Store Connect, uploading unless told not to.
destination=upload
if [ "${RAE_IOS_UPLOAD:-1}" = "0" ]; then
  destination=export
fi
options="$gen/ExportOptions.plist"
cat > "$options" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>method</key><string>app-store-connect</string>
  <key>destination</key><string>$destination</string>
  <key>teamID</key><string>$team</string>
  <key>signingStyle</key><string>automatic</string>
  <key>uploadSymbols</key><true/>
  <key>manageAppVersionAndBuildNumber</key><false/>
</dict>
</plist>
EOF
rm -rf "$gen/export"
echo "upload-ios: exporting ($destination)..."
# shellcheck disable=SC2086
xcodebuild -exportArchive -archivePath "$RAE_IOS_ARCHIVE" -exportOptionsPlist "$options" \
  -exportPath "$gen/export" -allowProvisioningUpdates $auth > "$gen/export.log" 2>&1 \
  || { tail -30 "$gen/export.log" >&2; echo "error: export failed (full log: $gen/export.log)" >&2; exit 1; }
if [ "$destination" = "upload" ]; then
  echo "upload-ios: uploaded $RAE_IOS_BUNDLE_ID ${RAE_IOS_VERSION:-1.0} (${RAE_IOS_BUILD}); it shows in App Store Connect -> TestFlight once processed"
else
  echo "upload-ios: exported to $gen/export (not uploaded)"
fi
