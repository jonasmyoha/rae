# Push notifications (lib/PushNotification)

A notification that the app's server sends to one device: a chest is ready, a friend wrote, it is
your turn in a match. The phone shows it also when the app is in the background or not running.
On iOS it goes through APNs (Apple Push Notification service), on Android through Firebase Cloud
Messaging (FCM). Rae gives the app the device token
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

A `PushNotification` has `payload`, the JSON the server sent, and `tapped`. On iOS the payload is
the whole `userInfo`, with its `aps` dictionary and any keys the server added; on Android it is the
message's `data` keys (see the Android section).

## What to expect

- **The permission is LocalNotification's.** One question covers both: call
  `localNotificationAsk()` (on Android 13 and later it asks for `POST_NOTIFICATIONS`). Registering does not ask anything. The token comes even when the person
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
  (`EventLoop.wake`). On iOS the token and the notifications come through the main thread's run
  loop, so they arrive while the app pumps its events (each frame), not during a long blocking
  call in `main`. At most 64 wait; more are dropped until the app reads them.
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

## Android: what the app supplies

On Android it is Firebase Cloud Messaging (FCM). The server sends through FCM's HTTP v1 API with
the token from `pushNotificationToken()`.

- **The app's `google-services.json`,** downloaded from its Firebase project (Project settings, the
  Android app whose package name is the app's `RAE_ANDROID_PACKAGE`). It is the app's secret: keep
  it outside the repository, or gitignored, never committed. Build with
  `RAE_ANDROID_GOOGLE_SERVICES=<path to it> sh tools/android/gen-android.sh <project>`. The build
  turns it into the string resources Firebase reads when it starts (`google_app_id`,
  `gcm_defaultSenderId`, `google_api_key`, `project_id`), as Google's Gradle plugin does.
- **The server's FCM service-account key** (a JSON key of the Firebase project). It belongs to the
  server, never to the app or to git.
- Without `RAE_ANDROID_GOOGLE_SERVICES` an app that imports lib/PushNotification still builds, with
  a note, but without Firebase: `pushNotificationRegister()` answers false, and
  `pushNotificationError()` says it was built without Firebase.
- **The permission** is the one question of Android 13 and later, `POST_NOTIFICATIONS`, asked by
  `localNotificationAsk()` (`runtime_local_notification_android.h`, SDL's permission request).
  Before Android 13 it is allowed without a question. The build declares it.
- **A device or emulator with Google Play services.** A `google_apis` emulator image has them.

The payload on Android is a JSON object of the message's `data` keys (FCM's data values are
strings). One that arrives while the app runs also has `"notification": { "title": ..., "body":
... }` when the message had a notification part; the system shows nothing then. When the app is in
the background or not running, FCM shows the notification itself, and its tap starts the app with
the data keys, which come with `tapped` true (Firebase's own `google.*`, `gcm.*`, `from` and
`collapse_key` are left out). A message with only `data` that arrives in the background reaches the
app only when it next runs in the foreground: waking the app in the background is not supported.

### How the build gets Firebase without Gradle

`tools/android/firebase/artifacts.lock` pins Firebase Messaging 24.1.2 and the 60 libraries it
depends on, each with its sha256. `resolve.py` writes it from the POMs on Google's Maven
repository and Maven Central, keeping the highest version of each, as Gradle would. Run it again
only to move the pin. `fetch.sh` downloads those files once into
`~/.cache/rae/android-firebase/<hash of the lock>/`, checks each sum and unpacks the AARs. Then
`gen-android.sh`, for an app whose C includes the push header:

- compiles the libraries' resources as overlays, with the app's Firebase values, and has `aapt2`
  write the R classes of every library package;
- merges the libraries' manifests into the app's (`merge-manifest.py`: their permissions and
  components, one `ComponentDiscoveryService` with every registrar), and declares
  `org.rae.app.push.RaePushService` for the messages;
- compiles `tools/android/firebase/java` (`RaePushService`: the token, the messages, `register`)
  against the jars, and turns all of it into `classes*.dex`.

## How it works

`compiler/runtime/runtime_push_notification.h` holds what the system hands over until Rae asks,
under a lock: the token, the last error and the waiting notifications. On iOS it adds the two
registration callbacks (`application:didRegisterForRemoteNotificationsWithDeviceToken:` and the
failure one) to SDL's app delegate, and in `application:willFinishLaunchingWithOptions:` sets a
`UNUserNotificationCenter` delegate. That delegate keeps the foreground empty, as iOS does with no
delegate, and passes on the notifications whose trigger is a `UNPushNotificationTrigger`.
On Android, `RaePushService` (a `FirebaseMessagingService`) and `RaeActivity` (the tap's
intent extras, in `onCreate` and `onNewIntent`) call into the same header through JNI, and
`rae_ext_PushNotification_register` calls `RaePushService.register()`, which asks
`FirebaseMessaging` for the token. `lib/PushNotification.rae` reads them and hands them out.

## What has been run

The headless path on a Mac (case 1706), the shim compiled for iOS arm64 with `-Wall -Wextra
-Werror`, and the module type-checked for every target (check-targets).

On the iOS Simulator (iPhone 16 Pro, iOS 27, 2026-11-27; `RAE_IOS_SDK=iphonesimulator`
gen-ios.sh, a window app that pumps its events each frame): `pushNotificationRegister()` got a real
APNs token (80 bytes, hex) within a second, and a notification sent with `xcrun simctl push`
while the app was in front came at once as `tapped=false` with the whole `userInfo`
(`{"aps":{"alert":{...}},"chest":"ready",...}`). A first probe that blocked `main` in a `sleep`
loop got neither until it returned: on iOS the Rae `main` runs on the main thread, and the token
and the delegate's calls come through its run loop. An app gets them while it pumps its events
(each frame), never inside a long blocking call. Not yet run on an iPhone: a tap from the
background and a tap that launches the app (a simulator banner cannot be tapped from a script),
and a send through APNs with a `.p8` key.

On an Android 15 emulator with Google Play services (2026-11-27), with a placeholder
`google-services.json` (a made-up project): Firebase starts from the values, the ask answers and
`localNotificationAllowed()` is true once the permission is granted, `pushNotificationRegister()`
reaches the Firebase servers, which refuse the made-up project (`pushNotificationError()`:
`FIS_AUTH_ERROR`), and an intent with a message's extras comes as `tapped=true
payload={"chest":"ready","match":"7"}`. A build without the JSON answers false with its error. Not
yet run with a real Firebase project: a real token and a message sent through FCM.
