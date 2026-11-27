# Push notifications (lib/PushNotification)

A notification that the app's server sends to one device: a chest is ready, a friend wrote, it is
your turn in a match. The phone shows it also when the app is in the background or not running.
On iOS it goes through APNs (Apple Push Notification service). Rae gives the app the device token
and the notifications that reach it. Sending them is the server's job, and is the app's own code.

```rae
open LocalNotification
open PushNotification

localNotificationAsk()                       # the permission is the same one
var pushNotifications: PushNotifications = createPushNotifications()  # on the World or App
pushNotificationRegister()                   # at each launch
...                                          # each frame:
if let token: String = pushNotificationToken() { ...send it to the server when it changed... }
if let push: PushNotification = nextPushNotification(pushNotifications: pushNotifications) {
  if push.tapped { ...open the screen the payload names... }
}
```

| call | what it does |
|---|---|
| `pushNotificationRegister()` | asks the system for this device's token, without waiting; true when it was asked |
| `pushNotificationToken()` | the token (hex on iOS), `none` until it has come |
| `pushNotificationError()` | why the last registration failed, or `none` |
| `nextPushNotification(pushNotifications:)` | the next notification, one per call, or `none` |
| `simulatePushNotification(payload:, tapped:)`, `simulatePushToken(token:)` | the system's part, for a test |

A `PushNotification` has `payload`, the JSON the server sent (on iOS the whole `userInfo`, with its
`aps` dictionary and any keys the server added), and `tapped`.

## What to expect

- **The permission is LocalNotification's.** On iOS one question covers both: call
  `localNotificationAsk()`. Registering does not ask anything. The token comes even when the person
  said no, but then the phone shows nothing.
- **The token comes later and can change.** Register at each launch, and send the token to the
  server when it differs from the one the server has. A token from a development build works only
  with APNs' sandbox.
- **A notification that arrives while the app is in the foreground** comes with `tapped` false, and
  the system shows nothing (as for a local notification). The app shows it its own way.
- **A notification the person tapped** comes with `tapped` true: one that opened the app from the
  background, and one that launched it. The launch one waits for the first call, as AppLinks does
  for the link that opened the app.
- **A notification that arrives while the app is in the background or not running** is shown by
  the system and reaches the app only if the person taps it. Waking the app in the background for a
  silent push (`content-available`) is not supported.
- Each notification comes once, whole, in the order they came, and wakes the frame loop
  (`EventLoop.wake`). At most 64 wait; more are dropped until the app reads them.
- Only remote notifications come here. A local notification (LocalNotification) does not.

## Headless and tests

In a headless run (`RAE_UI_HEADLESS`, `RAE_SDL_HEADLESS_MS` or `RAE_HEADLESS_FRAMES` set),
`pushNotificationRegister()` logs `[push] register`, answers false and gives a simulated token at
once: `RAE_PUSH_TOKEN` if set, else `simulated-push-token`. On every platform, `RAE_PUSH_OPENED=<json>`
stands in for a tap that launched the app: it is handed over at the first `nextPushNotification`
call, with `tapped` true. `simulatePushNotification` hands over one more. Case 1706.

## iOS: what the app supplies

- **The Push Notifications capability** on the App ID (Certificates, Identifiers & Profiles, or
  Xcode's Signing & Capabilities with automatic signing). An app whose C includes the shim gets the
  `aps-environment` entitlement from `tools/ios/gen-ios.sh`: `development` by default, the sandbox
  APNs that a build run from Xcode uses, and `RAE_IOS_PUSH_ENVIRONMENT=production` for TestFlight
  and the App Store. Without the capability, signing fails.
- **An APNs key for the server** (a `.p8` auth key, its key id and the team id), made in the Apple
  developer account. It belongs to the server, never to the app or to git.
- The simulator gets no real token: `pushNotificationError()` says why.

## Android (FCM): not yet

On Android, Firebase Cloud Messaging is not done yet; today Android, the Mac and the web register
nothing and receive nothing (`pushNotificationRegister()` answers false). When it comes, the app
will supply its own Firebase config (`google-services.json`, or its project id, app id and API key),
read at build time from a path outside the repository or from the environment. That file and the
server's FCM service-account key are an app's secrets: keep them out of git (in `.gitignore`), as
for every key.

## How it works

`compiler/runtime/runtime_push_notification.h` holds what the system hands over until Rae asks,
under a lock: the token, the last error and the waiting notifications. On iOS it adds the two
registration callbacks (`application:didRegisterForRemoteNotificationsWithDeviceToken:` and the
failure one) to SDL's app delegate, and in `application:willFinishLaunchingWithOptions:` sets a
`UNUserNotificationCenter` delegate. That delegate keeps the foreground empty, as iOS does with no
delegate, and passes on the notifications whose trigger is a `UNPushNotificationTrigger`.
`lib/PushNotification.rae` reads them and hands them out.

## What has been run

The headless path on a Mac (case 1706), the shim compiled for iOS arm64 with `-Wall -Wextra
-Werror`, and the module type-checked for every target (check-targets). Not yet run on an iPhone
with a real APNs send.
