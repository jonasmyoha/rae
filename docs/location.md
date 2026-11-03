# Location: where the device is

`lib/Location` finds the device's current place once, for things such as
the local weather. It asks for permission the first time ("when in use"),
then asks for one fix, and gives the answer as a value.

## The values

- `LocationFix`: `latitude` and `longitude` in degrees, `accuracyMeters`,
  `epochMs` (when it was measured, ms since 1970) and `reduced`.
- `reduced` is true when the person chose an approximate location for the
  app (iOS 14 and macOS 11 ask). Such a fix is still a fix; its accuracy is
  a few kilometers, enough for a weather lookup.
- `LocationAnswer`: `failure` is `none` and `fix` holds the place, or
  `failure` says why there is none and `message` has words for it:
  - `denied`: the person said no, now or before (or a parental control
    forbids it);
  - `off`: Location Services are off for the whole device;
  - `timedOut`: no answer within the request's `timeoutSec`;
  - `notAvailable`: no Core Location on this platform, or a headless run;
  - `failed`: Core Location could not tell (no signal) or another error.

## Asking

```rae
var request: LocationRequest = createLocationRequest(timeoutSec: 15.0)
startLocation(request: request, now: Time.now().epochSeconds())
# each frame, on the app's own state:
if let answer: LocationAnswer = pollLocation(request: request, now: Time.now().epochSeconds()) {
  if answer.failure is LocationFailure.none {
    # answer.fix.latitude, answer.fix.longitude
  }
}
```

`LocationRequest` is a resource the app keeps (no globals).
`request.accuracyMeters` is what to ask for (1000 by default).
`locationPending(request:)` is true while the request waits, so an
event-driven loop keeps a heartbeat for the timeout.

## Not blocking

`startLocation` asks Core Location and returns at once. Core Location
answers on the frame loop's run loop (the main thread); its delegate records
the answer and wakes the loop (`EventLoop.wake`), and the next
`pollLocation` hands it over, exactly once. A program with no run loop
(a command line tool) never hears back and times out.

## Platforms

- **iOS**: Core Location. tools/ios/gen-ios.sh links CoreLocation for an
  app that uses this module and writes `RAE_IOS_LOCATION_USAGE` into
  Info.plist as `NSLocationWhenInUseUsageDescription`. It refuses to
  generate the app without that text.
- **macOS**: Core Location. macOS asks only an app bundle whose Info.plist
  has a usage text (`NSLocationUsageDescription`); `rae run` builds no
  bundle, so a program run that way is denied or never answered.
- **Elsewhere**: `notAvailable` at once.
- **A headless run** (RAE_UI_HEADLESS, RAE_SDL_HEADLESS_MS or
  RAE_HEADLESS_FRAMES): `notAvailable` at once; Core Location is not
  touched.

## The C side

`compiler/runtime/runtime_location_apple.h`, included with `cheader`: one
Core Location call per function, plus the manager and its delegate, made
once and kept because Core Location answers through them later. The
delegate only records the fix or the CLError code. What a status or an
error means, the timeout and the order of the steps are Rae.

Case 1320 checks the values, the status and error meanings, a headless
answer and the timeout.
