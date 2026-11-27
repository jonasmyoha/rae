# App tracking and install attribution (lib/AppTracking)

iOS lets an app use the advertising id (IDFA) only after the person allows tracking, through App
Tracking Transparency (ATT). SKAdNetwork reports which ad led to an install without any id: the
app sets a conversion value, and Apple sends the ad network a postback with it. `lib/AppTracking`
gives both, on iOS only.

```rae
open AppTracking

if appTrackingStatus() is TrackingStatus.notDetermined {
  requestAppTracking()                     # once the app is in front, for example after a tutorial
}
...
if let advertisingId: String = appAdvertisingId() { ...send it with the analytics... }
updateConversionValue(fineValue: 12, coarseValue: ConversionCoarseValue.medium, lockWindow: false)
```

| call | what it does |
|---|---|
| `appTrackingStatus()` | `notDetermined`, `restricted`, `denied` or `authorized` |
| `requestAppTracking()` | asks the person the first time, without waiting; true when the system was asked |
| `appAdvertisingId()` | the IDFA when the status is `authorized`, else `none` |
| `updateConversionValue(fineValue:, coarseValue:, lockWindow:)` | SKAdNetwork's conversion value for this install's postback |

## The state machine

- **`notDetermined`** until the person answers. `requestAppTracking()` shows iOS's question with
  the app's purpose string; the answer arrives later, so read `appTrackingStatus()` again (the next
  frame, or when the app is active again). iOS shows the question only while the app is active:
  a request during launch, before the first frame, may show nothing.
- **`authorized`** or **`denied`** after the answer. iOS asks once per install: a later request
  shows nothing and the status stays. The person changes it in Settings, which the next
  `appTrackingStatus()` reads.
- **`restricted`** when the app may not ask (parental controls, a managed device). A request does
  nothing.
- **The advertising id** is there only in `authorized`. In any other state iOS gives all zeros,
  which `appAdvertisingId()` answers as `none`.

## SKAdNetwork conversion values

`fineValue` is 0 to 63 (six bits; the app decides what they mean, for example a level reached or a
purchase made). A value out of range is refused and sends nothing. `coarseValue` (`low`, `medium`,
`high`) is what Apple sends when too few installs came from the campaign for the fine value.
`lockWindow: true` ends the current postback window now, so the postback goes without waiting for
its end. Within one window a value may only grow: iOS keeps the higher one.

On iOS 16.1 and later all three go (`updatePostbackConversionValue:coarseValue:lockWindow:`). On
iOS 15.4 to 16.0 only the fine value goes; before 15.4 the old `updateConversionValue:`. The call
answers true when iOS took it. The ad networks an app works with are its own Info.plist's
`SKAdNetworkItems`, which the ad network's documentation lists; this module does not add them.

## What the app supplies

- `RAE_IOS_TRACKING_USAGE` for `tools/ios/gen-ios.sh`: the words iOS shows in the question, what
  the tracking is for. It becomes `NSUserTrackingUsageDescription`. iOS ends an app that asks
  without it, so the build stops with an error when an app that imports `lib/AppTracking` does not
  set it. The build also links AppTrackingTransparency, AdSupport and StoreKit.
- The App Store privacy answers for tracking, in App Store Connect.

## Headless and other platforms

Elsewhere (Android, the Mac, the web) the status is `notDetermined`, the request and the updates
do nothing and answer false, and there is no id. In a headless run (`RAE_UI_HEADLESS`,
`RAE_SDL_HEADLESS_MS` or `RAE_HEADLESS_FRAMES` set) each call logs `[tracking] <what>` and the
person's answer is simulated: `RAE_TRACKING_ANSWER` (`authorized`, `denied` or `restricted`;
`denied` when unset) becomes the status at the first request and stays, as on iOS; `authorized`
gives the id `00000000-0000-4000-8000-000000000001`. Case 1707 walks the states.

## What has been run

The headless state machine on a Mac (case 1707), the shim compiled for iOS arm64 with `-Wall
-Wextra -Werror`, every target type-checked (check-targets), and on the iOS Simulator (iPhone 16
Pro, iOS 27, 2026-11-27): the build writes the purpose string and links the frameworks, the status
reads `notDetermined`, a conversion value update is taken, and the request is made. Not yet run:
an answer to the question (a simulator dialog cannot be answered from a script) and an iPhone.
