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
if let allowed: Bool = localNotificationAllowed() { ... }
```

| call | what it does |
|---|---|
| `localNotificationAsk()` | asks the person the first time, without waiting; afterward the system answers from what they chose |
| `localNotificationSchedule(id:, atEpochMs:, title:, body:)` | one notification at that wall-clock moment, with the default sound |
| `localNotificationCancel(id:)` | cancels that one, if it is still to come |
| `localNotificationCancelAll()` | cancels all those still to come |
| `localNotificationClearDelivered()` | removes those in Notification Center, for when the app comes back |
| `localNotificationAllowed()` | `true`, `false` when the person said no, `none` when no ask has been answered in this run |

Each but the last answers true when the system was asked.

## What to expect

- **The moment is by the real clock, never `RAE_NOW`,** since the phone rings by its own clock. A
  moment in the past schedules nothing and answers false.
- **The same id replaces** the one still to come.
- **While the app is in the foreground the system shows nothing.** That is iOS's default for an app
  with no delegate, and such a notification does not reach Notification Center either. So an app
  shows its own alarm while it runs.
- **Ask first. What is scheduled after an ask and before its answer is held,** in order with any
  cancel, and added when the answer comes; it still rings at its moment, and one whose moment passes
  while the question is open is dropped. It is held in memory, so ending the app during the question
  loses it. Ask at each start: a schedule with no ask before it in this run is not held, and on a
  fresh install the system, as far as known, refuses it.
- **`true` does not mean it will ring.** If the person said no, or was not asked yet, the system
  shows nothing. `localNotificationAllowed()` tells an app when to say so; asking again when the app
  comes back picks up a change made in Settings, without showing the question again. Focus modes
  such as Sleep can hide the banner and the sound; the notification still reaches Notification Center.
- **The trigger is a length of time from now,** so it rings at that moment even if the person
  changes the time zone. iOS keeps the 64 soonest pending notifications of an app.
- iOS needs no purpose string for this. `tools/ios/gen-ios.sh` links UserNotifications for an app
  whose C includes the shim, `runtime_local_notification_apple.h`.

## Other targets

Elsewhere (the Mac, the web) every call does nothing and answers false, and
`localNotificationAllowed()` answers `none`. In a headless run (`RAE_UI_HEADLESS`,
`RAE_SDL_HEADLESS_MS` or `RAE_HEADLESS_FRAMES`) they answer the same and log a line a test can see:
`[local-notification] schedule <id> at <ms>`.

## Checks

Cases 1352 and 1353 hold the plain parts. Only a phone can show one ringing, so
`checks/localNotification` is for that:

```sh
RAE_IOS_TEAM=<team> sh tools/ios/run-ios.sh checks/localNotification
```

Delete the app from the phone first, so the question shows again. The check schedules while the
question is open. Tap Allow within 45 s, end the app, and look 45 s after it started: "rang with the
app closed" and "replaced, the new one" must be there at that moment, and neither line starting with
WRONG. The screen is gray until the answer, then green for yes and red for no.

The holding and `localNotificationAllowed()` have been built for a phone but not yet run on one.
