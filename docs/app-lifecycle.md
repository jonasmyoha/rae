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
| Android | leaving: `willEnterBackground` and `didEnterBackground` in a frame as the app goes (the window already counts as not visible), and the app runs on; back: `willEnterForeground`, `didEnterForeground`. A message sent in that frame arrives; Android itself cuts a background app's sockets after about 5 s ("Software caused connection abort"), so an app reconnects when it is back. |
| macOS, Windows, Linux, the browser | none: the app is always in front (`appInForeground` true). |

## Android: the app runs on in the background

The runtime turns off SDL's `SDL_HINT_ANDROID_BLOCK_ON_PAUSE` (on by default, it stopped the
app's thread in the event pump that sent the background events, so the program read them only when
it was back). In the background:

- there is no surface: `Gpu2d.windowVisible()` is false, so `shouldRenderFrame` / `renderGate`
  skip the drawing and Rae's frame acquires nothing;
- `Gpu2d.pollClose` lets the first three frames run at once, then waits up to a second for an
  event before each (a socket's wake, the foreground): a loop that draws every frame runs about
  once a second, at 0% CPU between;
- the runtime's SDL audio device is paused (SDL paused it only when it blocked), and plays again
  when the app is back unless it is muted;
- coming back, a new surface is made for the window Android shows now (asked from Java,
  `SDLActivity.getNativeSurface`: SDL's window property points at the freed window between the old
  surface and the new one), and `Gpu2d.windowResized()` says so, so an app that draws only on a
  change draws it. If the app was busy (loading) while it went away and came back, the surface is
  made at its next frame.

106 draws its page again on `didEnterForeground` (it used to stay black until touched).

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
- Android 15 emulator: a test app with a socket to this Mac reads `willEnterBackground` in a frame
  as it leaves and sends a message that arrives; back after 4 s, its next message arrives too
  (after 10 s Android had cut the socket). 106 and 136: 0.0% CPU in the background, drawn again on
  return; 136 also when sent away while still loading (where SDL's stale window pointer crashed the
  first try of this).
