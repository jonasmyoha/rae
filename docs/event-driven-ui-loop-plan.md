# Event-driven UI loop — planning document

Status: **design — not yet implemented**.

Successor to the current poll-based approach (`setTargetFPS(30)` idle /
`setTargetFPS(60)` active). Goal: zero CPU when truly idle, sub-millisecond
input latency, and the dirty-detection plumbing we already have
(`componentTableGeneration`, `renderCacheDirty`, `transformSystemIfDirty`,
`layoutSystemIfDirty`) finally pays off.

## 1. Motivation

The mobile UI today runs at:
- 60 fps when something is changing (real work happening)
- 30 fps when idle (the polling rate that keeps mouse clicks reliable)

The 30 fps idle is necessary because raylib only reads input state when our
loop calls `updateInputSystem`. At 5 fps idle, clicks shorter than 200 ms were
swallowed entirely. So we trade CPU savings for input latency.

Both halves of that trade are imperfect:
- **CPU savings:** 30 fps idle still walks `renderSystem` 30 times per second,
  even when nothing changed.
- **Input latency:** ~33 ms worst-case wake on input — fine on desktop, less
  ideal on battery-conscious mobile.

The standard solution in native UI toolkits (GTK, Qt, AppKit) is to block on
the OS event queue until something arrives. GLFW exposes this directly:

| Function | Behaviour |
|---|---|
| `glfwPollEvents()` | non-blocking; the current model |
| `glfwWaitEvents()` | **blocks** until any event arrives; CPU ≈ 0 % |
| `glfwWaitEventsTimeout(s)` | blocks until event OR timeout — supports animations |
| `glfwPostEmptyEvent()` | wakes a thread blocked in WaitEvents (from anywhere) |

This document plans how to land that model on top of Rae + raylib without
breaking the existing examples.

## 2. End-to-end behaviour

### Idle (no animation, no input)
```
loop: waitEventsTimeout(idle_cap_sec) → returns when:
         (a) OS delivers a mouse / keyboard / window event, OR
         (b) idle_cap_sec elapses (safety wake, e.g. 30 s).
      In case (a): poll input, dispatch actions, render if dirty.
      In case (b): poll the file-watch, render if dirty, sleep again.
```
**CPU profile:** the thread is `nanosleep`'d by the OS. Tools report ~0 %.

### Active (animation running)
```
loop: waitEventsTimeout(0.016) → returns when:
         (a) the 16 ms timeout fires (next animation frame), OR
         (b) input arrives early (treated as a normal interaction).
      In either case: poll input, advance animation, render.
```
**CPU profile:** roughly equivalent to the current 60 fps active path, minus
the throttle-loop bookkeeping.

### Click on otherwise-idle UI
```
loop blocked in waitEventsTimeout(30.0) →
  mouse-down event arrives → returns immediately (latency ~1 ms) →
  updateInputSystem sees the press → dispatchActions fires the OnClick →
  renderCacheDirty is true → render → loop iterates, waits again.
```
**Result:** input latency dominated by the OS event dispatch (~1 ms),
not our polling rate.

### Hot-reload of `.raescene`
The file-watch polling stays inside the loop. The `idle_cap_sec` (e.g. 30 s)
is the *maximum* time before the watcher checks; typical case is much sooner
because user activity or animations wake the loop. For a true zero-poll
hot-reload we'd need FSEvents (macOS) / inotify (Linux) bindings; that's a
follow-up, not part of this plan.

## 3. Layering

Three layers, each with a clear responsibility.

### 3a. C runtime — new natives (Layer A)

**Files:** `compiler/runtime/rae_runtime.c` (Compiled backend) and
`compiler/src/vm_raylib.c` (Live VM). Both share the trivial GLFW call.

```c
// compiler/runtime/rae_runtime.c
// (glfw3.h is already pulled in transitively via raylib's link line)
#include <GLFW/glfw3.h>

void rae_ext_waitEventsTimeout(double seconds) {
  glfwWaitEventsTimeout(seconds);
}

void rae_ext_waitEvents(void) {
  glfwWaitEvents();
}

void rae_ext_postEmptyEvent(void) {
  glfwPostEmptyEvent();
}
```

The Live VM `vm_raylib.c` mirrors these as `native_*` entries in its dispatch
table, with the same single-call body.

**Why three, not just `waitEventsTimeout`:**
- `waitEvents` for a future "guarantee zero CPU between user actions" mode
  (e.g. an editor that has no animations at all).
- `postEmptyEvent` for any future thread that needs to wake the main loop
  (file-watch on a separate thread, network response handler, etc.).
- They're all one-liners; binding all three now is free.

**Constraint:** the natives must be called *after* `initWindow()` — that's
what initialises GLFW. If called before, the underlying GLFW call is a no-op
on most platforms but undefined behaviour on others. Document this.

### 3b. Rae stdlib — bindings + a small policy helper (Layer B)

**Files:**
- `lib/raylib.rae` — extern declarations for the three natives.
- `lib/ui/loop.rae` — *new file*, tiny policy helpers. Not strictly
  required; the user code could compute timeouts inline. But a default
  policy makes the common case 2 lines of glue.

`lib/raylib.rae` additions:
```rae
# Blocks until a window/input event arrives or `seconds` elapses.
# Must be called only after `initWindow()`. `seconds` may be 0 (poll
# once and return) or a large value (wait until input). Negative
# values wait forever — prefer the explicit `waitEvents()` for clarity.
func waitEventsTimeout(seconds: view Float) extern

# Blocks until a window/input event arrives. No timeout. Use this only
# when the app is guaranteed to have no running animations; otherwise
# `waitEventsTimeout` is the right call.
func waitEvents() extern

# Wakes any thread currently blocked in `waitEvents` /
# `waitEventsTimeout`. Safe to call from any thread (but Rae is
# single-threaded today, so practical use is limited). Useful if a
# future file-watch / network response runs on its own thread.
func postEmptyEvent() extern
```

`lib/ui/loop.rae` (new):
```rae
# Common timeout values. Apps that want a non-default cadence can
# import these as named constants or compute their own Float
# directly — the helper below is purely a convenience.
let frameTimeoutSec: Float pub = 0.016     # one 60 Hz frame
let watcherPollSec:  Float pub = 0.5       # file-watch poll cadence
let idleCapSec:      Float pub = 30.0      # safety wake (longer ok)

# Pick the smallest of the four named signals — the smallest timeout
# wins because the loop should wake on whichever fires first.
#
#   animating   → a transition / playback / etc. needs a frame soon.
#   mouseDown   → user is holding the mouse button; we want to catch
#                 release smoothly without depending on the OS event.
#   pollWatcher → the file-watch needs a poll within `watcherPollSec`.
#   else        → truly idle; wake at `idleCapSec` as a safety net.
#
# Returns seconds-to-wait. Caller passes the result to
# `waitEventsTimeout`.
func nextWaitTimeoutSec(
  animating:   view Bool
  mouseDown:   view Bool
  pollWatcher: view Bool
) pub ret Float {
  if animating   { ret frameTimeoutSec }
  if mouseDown   { ret frameTimeoutSec }
  if pollWatcher { ret watcherPollSec  }
  ret idleCapSec
}
```

The stdlib deliberately **does not** own the loop, because every app has a
slightly different loop structure (stress mode, screenshot mode, multi-screen
nav, hot-reload, …). It owns the timeout policy primitive and lets the app
compose.

### 3c. User code (e.g. `examples/98_mobile_ui/main.rae`) — the loop (Layer C)

The user code owns the actual `loop`. Today it's a ~70-line block. After this
change it shrinks slightly and changes shape.

**Conceptual diff** (not the actual code, just the structure):
```rae
loop running and switching is false {
  # NEW: block until something happens.
  let timeout: Float = nextWaitTimeoutSec(
    animating:   playback.isPlaying or heroIsAnimating(anim)
    mouseDown:   isMouseButtonDown(button: mouseButtonLeft)
    pollWatcher: true
  )
  waitEventsTimeout(seconds: timeout)

  # Same as before from here, EXCEPT no setTargetFPS calls, and we
  # only render when `active` is true.
  if windowShouldClose() { running = false }
  else if autoExit { running = false }
  else {
    updateInputSystem(input: input, world: world)
    ... dispatch + theme + screen swap + watcher ...
    advancePlayback(playback, now)
    applyPlaybackToWorld(world, playback)  # only when isPlaying
    let animating: Bool = updateHeroAnim(anim, now)

    let active: Bool = forceRender or renderCacheDirty(...) or
                       input.events.length > 0 or
                       input.pressedEntity is not lastPressed or
                       animating or isMouseButtonDown(...)

    if active {
      layoutSystemIfDirty(world, layoutCache)
      transformSystemIfDirty(world, transformCache)
      beginDrawing()
      clearBackground(color: bg)
      renderSystem(world, texReg)
      ... overlays + hero ...
      updateAndRenderDebugOverlay(debug, now, didRealWork: true)
      endDrawing()
      forceRender = false
      renderCacheDirty(world, renderCache)   # snapshot
    }
  }
}
```

**Removed:** `setTargetFPS` calls (and the `lastTargetFps` tracking), the
unconditional begin/render/end every iteration.

**Added:** the `waitEventsTimeout` call at the top of each iteration.

Notice the loop body is now genuinely event-driven: each iteration starts by
waiting, ends by deciding whether to draw, never busy-loops.

## 4. Wait-timeout policy in detail

The `nextWaitTimeoutSec` helper covers the common case. Apps with unusual
needs (debug heartbeat that wants to advance even when nothing else is
happening, network polling, etc.) can compute their own `Float` and call
`waitEventsTimeout` directly.

### Animations and the timeout

Animations are the trickiest part of event-driven loops. The pattern is:

1. Animation **starts** (e.g. user taps play): `dispatchActions` flips
   `playback.isPlaying = true`. *The current iteration is already past the
   wait;* it processes the event, sees `animating=true`, renders, loops back.
2. **Next iteration:** `nextWaitTimeoutSec(animating: true, …)` returns
   `frameTimeoutSec = 0.016`. The loop waits at most 16 ms.
3. Animation runs at ~60 Hz this way — same effective rate as the current
   throttle, but the loop wakes on input MID-frame too.
4. Animation **ends:** `playback.isPlaying = false`. Next iteration:
   `nextWaitTimeoutSec(animating: false, …) = idleCapSec = 30.0`. Loop
   blocks for up to 30 s.

The `frameTimeoutSec` is a *maximum*. If input arrives at, say, 5 ms in, the
wait returns early and we get a 5 ms frame. That's fine — the animation
advance is timestamp-based (`getTime() - prevTime`), not frame-count-based.

### `mouseDown` as a force-active signal

While the mouse button is held, we want to be at 60 Hz so the release edge is
caught immediately. `nextWaitTimeoutSec` reads `isMouseButtonDown` and
returns `frameTimeoutSec`. After release, the next iteration sees down=false
and falls back to whatever else is happening.

This is the same belt-and-suspenders we already have for clicks.

### `idleCapSec` justification

Why 30 s and not "wait forever"? Two reasons:
1. **File-watch polling.** The 30 s cap means the file watcher checks at
   least every 30 s even when the user is completely idle. For dev hot-
   reload this is fine; for production where no `.raescene` is being edited,
   it costs one syscall every 30 s, ~zero CPU.
2. **Diagnostic safety.** If a Rae bug ever caused the dirty-detection to
   stop firing, the 30 s wake guarantees the app at least repaints
   periodically rather than appearing frozen forever.

For an app with hot-reload disabled and no diagnostic concerns, `idleCapSec`
can be set to any large value (e.g. 600.0). The stdlib default is the safe
choice.

## 5. Edge cases

### 5a. First-frame after world rebuild
The `switching` flow rebuilds the world and re-enters the inner loop. The
first iteration's `forceRender = true` (set before the inner loop) guarantees
the first frame renders. The `renderCacheDirty` call after renders snapshots
the now-current generations into the cache, so the second iteration accepts
the cache as clean.

### 5b. `RAE_AUTO_EXIT_SEC`
The test-mode auto-exit currently fires inside the inner loop. With the new
wait, an iteration could be sleeping for `idleCapSec` (30 s) when the
auto-exit deadline arrives. Fix: clamp `timeout` to `(autoExitSec -
elapsedSec)` when auto-exit is active. Simple Float min at the call site.

### 5c. `RAE_UI_STRESS_REBUILDS`
The stress mode runs its own loop that doesn't render at all — it just
rebuilds the world N times. Untouched by this change.

### 5d. Snapshot mode (`RAE_UI_SCREENSHOT`)
Forces a render after N iterations. Today it relies on `setTargetFPS`
maintaining a frame rate. With wait-events, the snapshot loop can either:
- Force `active = true` for the snapshot iterations so the loop renders, OR
- Call `waitEventsTimeout(0.016)` deliberately to pace at 60 Hz.

Both work. Pick whichever is shorter in the migration commit.

### 5e. Heartbeat under wait-events
The debug heartbeat advances only on real-work frames (commit `242a983`).
With wait-events, real-work frames happen only when something dirties or
animations fire. The heartbeat will be naturally slower during pure-input
interactions, faster during animations. That matches the "is the host
doing meaningful work right now?" semantic.

### 5f. `windowShouldClose` event handling
Closing the window via the OS chrome (red X on macOS, alt-F4 on Windows)
generates a GLFW event that `waitEventsTimeout` will return on. The next
iteration sees `windowShouldClose() == true` and exits cleanly. No change
needed.

## 6. Live VM considerations

Three natives, three new extern function bindings on the Live VM side
(`vm_raylib.c`). Each is identical to the C-runtime version (single GLFW
call, no state). Should "just work" given the existing pattern for raylib
natives — no new VM mechanism, no new opcode.

The recent Live VM work (`OP_BIND_LOCAL_VALUE`, `OP_VIEW_LOCAL` pod inline,
`OP_BUF_REF`) doesn't affect this path; `waitEventsTimeout(seconds: Float)`
takes one primitive Float arg and returns nothing. As simple as natives get.

Test on both targets via the existing `make test` pipeline + an interactive
run of `98_mobile_ui` in Live mode.

## 7. Implementation phases

Each phase is one commit, with a separate submodule + outer-repo bump pair.

### Phase 1 — Bind the natives
- `compiler/runtime/rae_runtime.c`: add three native impls.
- `compiler/src/vm_raylib.c`: add three native dispatch entries.
- `compiler/src/vm_natives_core.c`: register the three names (if that's
  where Live VM names live).
- `lib/raylib.rae`: extern declarations.
- New test `tests/cases/491_wait_events_smoke`: opens a window, calls
  `waitEventsTimeout(seconds: 0.01)`, closes window, exits 0. Runs on
  both targets. Smoke test that the bindings work.

**Risk:** very low. New natives, no behaviour change anywhere else.

### Phase 2 — Add stdlib helper
- `lib/ui/loop.rae` (new file): the timeout constants and
  `nextWaitTimeoutSec` helper.
- No tests needed beyond a one-shot check that the file compiles
  (it'll be exercised by Phase 3's example migration).

**Risk:** trivial.

### Phase 3 — Migrate the mobile UI example
- `examples/98_mobile_ui/main.rae`: replace the `setTargetFPS` / always-render
  block with `waitEventsTimeout` + conditional render.
- Drop the `lastTargetFps` state.
- Honour `RAE_AUTO_EXIT_SEC` via timeout clamping.
- Verify visually that:
  - Interactive clicks register correctly.
  - Playback / hero animations are smooth.
  - Theme toggle works.
  - Idle CPU drops to near 0 (use `time …` over a 10 s window with the app
    untouched).
- Run the full test suite. Snapshot the Live and Compiled "album closed"
  PNGs and compare.

**Risk:** moderate — refactor of a busy file. Mitigated by interactive
verification + snapshots.

### Phase 4 — Documentation + retrospective
- Update `docs/event-driven-ui-loop-plan.md` (this file) to mark complete.
- Update `docs/ownership-model.md` final-verification table with the new
  idle CPU numbers.
- Optional: capture a one-paragraph note in `docs/GOALS.md` about the
  "low idle CPU" property now being real, not aspirational.

**Risk:** none.

## 8. Testing plan

Unit tests stay the same (none of them touch the loop). Visual /
performance acceptance:

| Check | Method | Target |
|---|---|---|
| Idle CPU (Live, 10 s) | `time` with `RAE_AUTO_EXIT_SEC=10` | < 0.05 s user / < 1 % |
| Idle CPU (Compiled, 10 s) | same | < 0.10 s user (excluding compile) |
| Click latency | interactive | feels instant; no missed clicks across 20 attempts |
| Playback animation | interactive | smooth, no stutter |
| Hero transition | interactive | smooth |
| Theme toggle (T key) | interactive | works first try |
| Hot-reload (.raescene edit) | edit + observe | updates within 1 s |
| 20k stress | `RAE_UI_STRESS_REBUILDS=1 RAE_UI_STRESS_N=20000` | same plateau as before |
| Full suite | `make test` | 327 unit + 50 example green |

If any of the perf targets isn't hit on the first try, the most likely
culprit is the timeout policy (too low an idle cap, or `mouseDown` not
gating correctly). Tune `nextWaitTimeoutSec`, not the underlying mechanism.

## 9. Out of scope (for now)

- **FSEvents / inotify** for zero-poll file watching. Adds platform-specific
  natives, deferred.
- **Threaded input or threaded animation.** Rae is single-threaded today.
- **Cocoa beachball protection.** macOS still expects events to be drained
  within ~5 s; `waitEventsTimeout` with a 30 s cap is fine because the OS
  itself wakes us when it has events for us (mouse move, focus change,
  command-tab). The 30 s cap is only the *max idle* in absence of OS events,
  not the actual time before the OS knows we're alive.
- **Render skip via offscreen RenderTexture caching.** With wait-events,
  the renderSystem cost is amortised over fewer frames (renders only happen
  when dirty). Texture caching becomes a refinement, not a necessity.

## 10. What we get when this lands

- Idle CPU effectively 0 % (was 19–45 % depending on rate).
- Input latency below the current 33 ms (depending on OS, sub-1 ms in many
  cases).
- The dirty-detection plumbing (`componentTableGeneration`,
  `renderCacheDirty`, `transformSystemIfDirty`, `layoutSystemIfDirty`)
  becomes load-bearing — it actually controls whether we render, not just
  how often we set a target FPS.
- The architectural endpoint of the work that started with the "switching"
  bool back in `examples/98_mobile_ui/main.rae` line 254.

## 11. Open questions to resolve before implementation

1. Are GLFW headers already on the include path for both the Compiled
   runtime and Live VM? If not, where do they live? (raylib bundles a
   GLFW build; check the existing include line.)
2. Are there any tests currently relying on `setTargetFPS` behaviour
   (timing-sensitive frame counts) that would break with wait-events? The
   stress test doesn't, the snapshot script doesn't — but worth a grep.
3. Does the Live VM's raylib native registration need any special
   handling for void-return natives that take a Float arg? (probably no —
   `setTargetFPS(Int)` and `getTime() ret Float` already exist and prove
   both shapes work.)

End of plan.
