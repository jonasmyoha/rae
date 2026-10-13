# tools/ios — running Rae on iOS

Design: `docs/ios-testing.md`. Tasks: `#516` (umbrella), `#518`–`#524`.

Rae's native target (Compiled: emit C → binary) ports to iOS by compiling the
generated `app.c` + the amalgamated `rae_runtime.c` for `arm64-apple-ios`, linking
static SDL3 + wgpu-native (Metal), and bundling assets. WASM is the other track
(a WKWebView launcher, `#523`).

## One-time: build the dependency xcframeworks

Both deps ship only as macOS builds; build the iOS versions once:

```sh
sh tools/ios/build-wgpu-ios.sh    # downloads matching v29.0.1.1 iOS libs → wgpu_native.xcframework
sh tools/ios/build-sdl3-ios.sh    # builds SDL release-3.4.10 from source → SDL3.xcframework
```

Both land in `tools/ios/frameworks/` (git-ignored — regenerate with the scripts).

## Generate + run one example on device

```sh
sh tools/ios/gen-ios.sh examples/102_gpu2d_animated
open build/ios/102_gpu2d_animated/proj/RaeApp.xcodeproj
```

In Xcode: **Signing & Capabilities → Team** (Automatic), pick your plugged
device, **Run**. Signing needs your Apple ID/team and can't be scripted.

The generator: emits the app C, prepends `<SDL3/SDL_main.h>` (so SDL owns the iOS
entry), bundles the example's `assets/` preserving relative paths, and writes a
CMake `-G Xcode` project. `ios_bootstrap.c` `chdir`s to the bundle at launch so
relative asset reads resolve.

Options, as environment variables: `RAE_IOS_BUNDLE_ID` / `RAE_IOS_NAME`,
`RAE_IOS_ORIENTATIONS=portrait|any`, `RAE_IOS_DEPLOYMENT_TARGET` (15.0),
`RAE_IOS_ENTRY`, and `RAE_IOS_ICON=<square 1024x1024 opaque PNG>` — the app
icon, made the AppIcon set of a generated asset catalog (single-size; Xcode
makes the rest).

## Upload to App Store Connect (TestFlight)

```sh
RAE_IOS_TEAM=<team-id> RAE_IOS_BUNDLE_ID=<id> RAE_IOS_VERSION=0.1.0 RAE_IOS_BUILD=7 \
  sh tools/ios/upload-ios.sh <project-dir>
```

Archives with automatic signing (Xcode makes the Apple Distribution
certificate and the App Store profile when the account may) and exports
straight to App Store Connect, as the Apple ID in Xcode → Settings → Accounts
or an App Store Connect API key (`RAE_IOS_ASC_KEY_PATH`, `RAE_IOS_ASC_KEY_ID`,
`RAE_IOS_ASC_ISSUER_ID`). Needs an explicit App ID and an App Store Connect app
for the bundle id first. `RAE_IOS_BUILD` must grow with every upload of a
version; `RAE_IOS_UPLOAD=0` stops at the `.ipa`.

Every generated app ships `PrivacyInfo.xcprivacy` (what the runtime and SDL3
use: file timestamps in the app's container, elapsed time; no tracking, no
collected data) — `RAE_IOS_PRIVACY_MANIFEST` replaces it.
`RAE_IOS_EXEMPT_ENCRYPTION=1` writes `ITSAppUsesNonExemptEncryption` NO (the
app's own declaration), and `RAE_IOS_VERSION` / `RAE_IOS_BUILD` set
`CFBundleShortVersionString` / `CFBundleVersion` (default 1.0 / 1).

## Status / next

- `build-wgpu-ios.sh`, `build-sdl3-ios.sh`, `gen-ios.sh`, `ios_bootstrap.c`: done.
- `#520`: first device run of `102` then `114` (validate SDL_main entry, the busy
  render loop vs iOS lifecycle, and touch input).
- `#521`: multi-example generator (one target per `.raepack featured` example,
  scheme dropdown = launcher).
- `#523`: WKWebView WASM launcher.
