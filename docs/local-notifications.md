# Local notifications (lib/LocalNotification)

A system notification at a later time, which rings also with the app in the background or not
running. On iOS it is UNUserNotificationCenter.

```rae
open LocalNotification

localNotificationAsk()
localNotificationSchedule(id: "task1", atEpochMs: when, title: "Milk", body: "Buy it")
localNotificationCancel(id: "task1")
localNotificationCancelAll()
localNotificationClearDelivered()
```

| call | what it does |
|---|---|
| `localNotificationAsk()` | asks the person the first time; the system remembers the answer |
| `localNotificationSchedule(id:, atEpochMs:, title:, body:)` | one notification at that wall-clock moment, with the default sound |
| `localNotificationCancel(id:)` | cancels that one, if it is still to come |
| `localNotificationCancelAll()` | cancels all those still to come |
| `localNotificationClearDelivered()` | removes those in Notification Center, for when the app comes back |

Each answers true when the system was asked.

## What to expect

- **The moment is by the real clock, never `RAE_NOW`,** since the phone rings by its own clock. A
  moment in the past schedules nothing and answers false.
- **The same id replaces** the one still to come.
- **While the app is in the foreground the system shows nothing.** That is iOS's default for an app
  with no delegate, and such a notification does not reach Notification Center either. So an app
  shows its own alarm while it runs.
- **`true` does not mean it will ring.** If the person said no, or was not asked yet, the system
  takes the request and shows nothing. Focus modes such as Sleep can hide the banner and the sound;
  the notification still reaches Notification Center.
- **The trigger is a length of time from now,** so it rings at that moment even if the person
  changes the time zone. iOS keeps the 64 soonest pending notifications of an app.
- iOS needs no purpose string for this. `tools/ios/gen-ios.sh` links UserNotifications for an app
  whose C includes the shim, `runtime_local_notification_apple.h`.

## Other targets

Elsewhere (the Mac, the web) every call does nothing and answers false. In a headless run
(`RAE_UI_HEADLESS`, `RAE_SDL_HEADLESS_MS` or `RAE_HEADLESS_FRAMES`) it answers false and logs a line
a test can see: `[local-notification] schedule <id> at <ms>`.

## Checks

Cases 1352 and 1353 hold the plain parts. Only a phone can show one ringing, so
`checks/localNotification` is for that:

```sh
RAE_IOS_TEAM=<team> sh tools/ios/run-ios.sh checks/localNotification
```

Tap Allow, end the app once it has scheduled (10 s), and look after 45 s: "rang with the app closed"
and "replaced, the new one" must be there, and neither line starting with WRONG.
