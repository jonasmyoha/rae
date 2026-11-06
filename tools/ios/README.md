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

Each `gen-ios.sh` run stages the project's assets afresh and removes the
`.app` an earlier build assembled, so a file deleted from the project never
stays in the next build (Xcode's resource copy into an existing bundle only
adds and updates). `sh tools/ios/gen-ios-smoke.sh` checks it on a
throwaway project, with no device or signing (about a minute).

The generator: emits the app C, prepends `<SDL3/SDL_main.h>` (so SDL owns the iOS
entry), bundles the example's `assets/` preserving relative paths (plus any
`examples/<other>/assets/...` file the app reads from another example, e.g. 118's
HUD font from 114), and writes a
CMake `-G Xcode` project. `ios_bootstrap.c` `chdir`s to the bundle at launch so
relative asset reads resolve.

Options, as environment variables: `RAE_IOS_BUNDLE_ID` / `RAE_IOS_NAME`,
`RAE_IOS_ORIENTATIONS=any|portrait` (default `any`: the app follows how the
device is held; `portrait` locks a phone upright), `RAE_IOS_DEPLOYMENT_TARGET` (15.0),
`RAE_IOS_ENTRY`, and `RAE_IOS_ICON=<square 1024x1024 opaque PNG>` — the app
icon, made the AppIcon set of a generated asset catalog (single-size; Xcode
makes the rest).

**The build folder: `RAE_IOS_BUILD_NAME`.** Everything is generated into
`build/ios/<name>/` (the Xcode project under `proj/`, `meta.env`, the archive
and the `.ipa`), where `<name>` is `RAE_IOS_BUILD_NAME`, by default the project
folder's name. Two projects whose folders share a generic name — SUMU's Rae
project is the folder `shell/`, another app's may be `app/` — would share
`build/ios/shell/` and overwrite each other's Xcode project, so each sets its
own: `RAE_IOS_BUILD_NAME=sumu sh tools/ios/gen-ios.sh ~/personal/sumu/sumu/shell`.
`gen-ios.sh`, `run-ios.sh` and `upload-ios.sh` all read it, so set the same
value for all three (a Makefile sets it once with the other `RAE_IOS_*`
options). It is one folder name: no `/`. It names only the folder: the
bundle id and the display name stay `RAE_IOS_BUNDLE_ID` / `RAE_IOS_NAME`.

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

**Purpose strings** (Info.plist; plain text, no `<`, `>`, `&` or `"`).
`RAE_IOS_BLUETOOTH_USAGE`, `RAE_IOS_CAMERA_USAGE` and
`RAE_IOS_MICROPHONE_USAGE` word the Bluetooth, camera and microphone texts.
They are always written, because SDL3 links those APIs and App Store
Connect refuses a build without them.

**Links that open the app.** `RAE_APP_LINKS` (`https` hosts: universal links)
and `RAE_APP_SCHEME` (a custom scheme) make a link open the app, which receives
it through lib/AppLinks. docs/app-links.md lists the file the website must serve.

**The local network.** An app that connects to a machine on the local
network (an address like `192.168.1.20`, a name like `mymac.local`) needs
`RAE_IOS_LOCAL_NETWORK_USAGE`, the text iOS shows when it asks the person.
Without it iOS refuses the connection and asks nothing. This covers
lib/net/NetworkConnection (Network.framework, which App Transport Security
does not cover). An app that browses or publishes Bonjour services lists their
types in `RAE_IOS_BONJOUR_SERVICES`, space-separated (`"_sumu._tcp"`, each
`_name._tcp` or `_name._udp`), which needs the text too. Both keys
(`NSLocalNetworkUsageDescription`, `NSBonjourServices`) are written only when
set, so other apps are unchanged:

    RAE_IOS_LOCAL_NETWORK_USAGE="Sync talks to your Mac on this network." \
      RAE_IOS_BONJOUR_SERVICES="_sumu._tcp" sh tools/ios/gen-ios.sh <project>

**The location.** An app that uses lib/Location (docs/location.md) needs
`RAE_IOS_LOCATION_USAGE`, the text iOS shows when it asks for the location
("The weather is shown for where you are."); gen-ios.sh refuses to generate
such an app without it, since iOS would never ask and App Store Connect
refuses the build. It is written as `NSLocationWhenInUseUsageDescription`
whenever it is set, and CoreLocation is linked only for an app that uses
lib/Location.

**Notifications at a later time.** An app that uses lib/LocalNotification
(docs/local-notifications.md) links UserNotifications; iOS needs no purpose
string for them.

`sh tools/ios/gen-ios-smoke.sh` checks both keys (absent by default, present
when set, the bad combinations refused) along with the staged assets. Like
gen-ios.sh it needs Xcode's iOS SDK: with only the Command Line Tools
selected, run it with
`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`.

## Testing on a device: what an agent can and cannot do

**An agent cannot close an iPhone app.** `xcrun devicectl device process
terminate --pid <pid>` prints "Sent signal", and the app stays on screen with the
same pid, so never report an app as closed from that output. To put an app in the
background, open another app such as Settings (`xcrun devicectl device process
launch --device <id> com.apple.Preferences`), or ask someone at the phone to go to
the Home Screen. Check the result by what the app logs or by asking what the
screen shows.

**A locked phone refuses every launch** (`CoreDeviceError` 10002, reason
`Locked`). A permission question also needs someone to tap it. So a check that
needs either one needs someone at the phone. Deleting the app
(`xcrun devicectl device uninstall app --device <id> <bundle-id>`) makes iOS ask
its questions again, as on a fresh install.

**Notifications** (lib/LocalNotification, full details in
docs/local-notifications.md):

- `localNotificationSchedule` asks iOS for one notification at a wall-clock
  moment. iOS rings it itself, also with the app in the background or not running.
- **iOS shows nothing while the app is in the foreground,** and the notification
  does not reach Notification Center either. So a test puts the app in the
  background before the moment. The app does not need to be closed.
- Call `localNotificationAsk()` at each start and when the app comes back. It does
  not wait. What is scheduled after it and before the answer is held and added
  when the answer comes, and still rings at its moment. It is held in memory, so
  ending the app during the question loses it.
- `localNotificationAllowed()` answers `true`, `false` when the person said no, or
  `none` when no ask has been answered in this run.
- `checks/localNotification` tries all this on a phone. On a fresh install, tap
  Allow within 45 s and leave the app: "rang with the app closed" and "replaced, the
  new one" must ring 45 s after the start, and no line starting with WRONG.
  The screen is gray until the answer, green for yes, red for no.

## Profile on a device

    APP=<bundle id> PLACES="name|x y" AT_ENV=<the app's start-spot variable> \
      COOL_WAIT=180 tools/ios/profile-ios.sh <device id> [seconds per place]

It stops at the first launch or log copy that fails and at a total time limit
(`TOTAL_SECONDS`, 30 minutes), says so with a `STOPPED` line and exits 1.
`tools/profile-desktop.sh` prints the same summary on a desktop, so start a comparison
there. The GPU slots in it are not costs: docs/profiling.md, "What to trust".

One run per place with GPU pass and frame timing on (the app's
app3d/FrameLog `[frames]` lines among them), cooling between runs if asked,
each log copied off the phone and summarized (median and 95th percentile
frame time, frames over budget, scales, thermal states, GPU passes, CPU
time; SUMMARY_FIRST / SUMMARY_MIDDLE add lines of the app's own).
`SUMMARY_ONLY=1` summarizes logs already copied. The script's head lists
every setting.

## App icons from a shot

    tools/ios/make-icons.sh <shot image> <icons dir>

A square cropped round a point of an app's own shot, brightened toward a
mean lightness (a gamma on Oklab's L; refused outside the band, washed out
or off-hue), written at every size iOS, Android and macOS want. The crop,
the band, the hue patch and the background are ICON_* settings.

## Status / next

- `build-wgpu-ios.sh`, `build-sdl3-ios.sh`, `gen-ios.sh`, `ios_bootstrap.c`: done.
- `#520`: first device run of `102` then `114` (validate SDL_main entry, the busy
  render loop vs iOS lifecycle, and touch input).
- `#521`: multi-example generator (one target per `.raepack featured` example,
  scheme dropdown = launcher).
- `#523`: WKWebView WASM launcher.
