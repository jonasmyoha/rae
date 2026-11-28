# The app's lifecycle: background and foreground

`lib/AppLifecycle` tells a program when the app leaves the screen and comes back, as the system
says it. A server that decides between an in-app toast and a push message needs to know (a
suspended iOS app can keep its socket open for minutes), and session analytics end a session
when the app leaves.

```rae
open AppLifecycle

loop true {
  if let change: AppLifecycleEvent = appLifecycleNext() {
    if change is AppLifecycleEvent.willEnterBackground {
      # tell the server, end the session
    }
  } else {
    break
  }
}
```

- `appLifecycleNext() ret opt AppLifecycleEvent`: the next change not read yet, oldest first
  (`willEnterBackground`, `didEnterBackground`, `willEnterForeground`, `didEnterForeground`).
  Read until none once a frame.
- `appInForeground() ret Bool`: false from `willEnterBackground` to `didEnterForeground`.
- Each change wakes the frame loop (`EventLoop.wake`), so a loop waiting in `Gpu2d.waitEvents`
  runs a frame at once.

The C side is `compiler/runtime/runtime_app_lifecycle.c`: SDL hands the lifecycle events to an
event watcher at once instead of queueing them, so the gpu2d runtime's watcher records each one in
a small ring (64; the oldest goes when the program has not read them) with whether the app is in
front, and wakes the loop.

## The platforms

| platform | what the program sees |
|---|---|
| iOS | `didEnterForeground` at launch; leaving: `willEnterBackground`, frames go on (about 1.5 s in the simulator), then `didEnterBackground`; back: `willEnterForeground`, `didEnterForeground`. The program reads `willEnterBackground` while it still runs, so it can send its last message. |
| Android | the four arrive, but only when the app is back: SDL stops the app's thread in the same event pump that sends the background pair (`SDL_HINT_ANDROID_BLOCK_ON_PAUSE`, the runtime releases the window's surface there), so no frame runs in the background. A program learns afterward that it was away. |
| macOS, Windows, Linux, the browser | none: the app is always in front (`appInForeground` true). |

## Simulated

`Gpu2dSimulatedInput.simulateEnterBackground()` pushes `willEnterBackground` and
`didEnterBackground` as SDL events, `simulateEnterForeground()` the foreground pair, through the
same watcher as a device's; without a window they go to the ring directly.
`Gpu2dSimulatedInput.wakePending()` says whether a wake waits in the window's event queue.

## Checked

- Case 1718 (headless, a window): a simulated background then foreground arrive in order, each
  once, `appInForeground` follows them, and each leaves a wake waiting.
- iOS Simulator: Home (another app launched) and back, with the frame number of each change: the
  background pair 13 frames apart, read while leaving.
- Android 15 emulator: Home and back twice: the four arrive together on return (above).
