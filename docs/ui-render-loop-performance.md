# UI render-loop performance — what we learned fixing `98_mobile_ui`

A postmortem of the work that took `examples/98_mobile_ui` from a janky
8–34 fps to fluid, near-perfect rendering. Two independent root causes,
several red herrings, and one durable architectural lesson.

## The headline lesson

> **A wait-based (event-driven) loop cannot reliably drive smooth
> continuous animation. A busy render loop can.**

An event loop that sleeps in `glfwWaitEventsTimeout(timeout)` and only
renders when it wakes must *predict* when to wake. For discrete UI (click,
type, hover) that's perfect and idles at ~0% CPU. For **continuous motion**
(scrolling, dragging, transitions) it's fragile: if the predicted wake
cadence is even slightly off, frames are skipped and animation stutters.
The tell was unmistakable — **scrolling capped at ~34 fps, but moving the
mouse at the same time pushed it to 60**, because mouse-move events were
the only thing waking the loop fast enough.

The fix is a **hybrid loop**:
- **Moving / interacting** → busy-render: poll (`waitEventsTimeout` with
  timeout `0`) and paint *every* iteration with no FPS cap. Render the next
  frame as fast as the machine allows.
- **Idle** → blocking event-wait (~0% CPU).

This is the best of both: buttery animation when it matters, zero CPU when
nothing moves.

## Is this fundamental to Rae UI? No — it's per-app

The render loop is **owned by each app**, not by `lib/ui`. `lib/ui`
provides the *systems* (`layoutSystem`, `transformSystem`, `renderSystem`)
and a timeout *policy* helper (`nextWaitTimeoutSec` in
`lib/ui/event_loop.rae`), but every app writes its own loop because each
has different structure (screenshot mode, hot-reload, navigation, …).

How the examples render:

| Example | Loop model | Why it was fine |
| --- | --- | --- |
| `91/92_pong`, `93_raylib_3d`, `94/97_tetris`, `95/96_easing`, `90_raylib_basic`, `100_font` | Classic busy game-loop: `setTargetFPS(60)` + `loop not windowShouldClose() { … beginDrawing/endDrawing }` | Renders **every** frame; never waits on events. Smooth by construction (capped at 60 by `setTargetFPS`). |
| `98_mobile_ui` | Event-wait (now hybrid) | Chose event-wait for idle efficiency — which is exactly what made continuous animation fragile. |

So the games were never affected: they already busy-render. Only the
mobile UI used the event-wait model, and only it needed this fix. If a
future app wants both idle efficiency *and* smooth animation, copy the
mobile UI's hybrid loop (busy while animating, wait while idle).

## The two real root causes

### 1. O(n²) per frame from a linear-scan component store (algorithmic)

`ComponentTable.componentIndexOf` was a **linear scan** over the dense
entity array, so every `componentGet`/`Has`/`View`/`Mod` was O(n). The
per-frame layout/transform/render walks do ~a dozen lookups per entity
across all entities, so each frame was **O(n²)**. At 200+ list items (each
several entities) that's millions of scan steps per frame — and being
*algorithmic*, no `-O2` vs `-O0` made any difference (this is why "compiled
release is as slow as ever").

**Fix:** a sparse map `sparse[entity.value] = dense index` makes lookup
O(1) (with a dense back-reference check so stale slots can't false-hit).
Dense-array manipulation and drop/ownership semantics are byte-for-byte
unchanged. The original code comment had anticipated exactly this
("swap to an IntMap-backed sparse set later"). Frame cost went O(n²) → O(n).

### 2. Frame-scheduling starvation (the busy-loop lesson above)

Even with O(1) lookups, the event-wait loop only requested frames at the
display rate when `animating` was true — and steady wheel scrolling
(`velocity == 0`, driven per-event) wasn't flagged as animating, so the
loop fell back to the 0.05s watcher poll (~20 Hz). Fixed first by counting
recent wheel activity as animating, then more fundamentally by switching to
the busy-render-while-interacting model.

## Everything we tried, in order (including the dead ends)

1. **Build profile (release vs debug)** — *red herring for the jank, but a
   real feature.* The compiled path was already `-O2`; the runtime has zero
   `assert()`s, so `-DNDEBUG` bought nothing. We still wired the previously
   dead `--profile dev|release` flag into `rae run`/`watch` (dev `-O0 -g`,
   release `-O2 -DNDEBUG`) and added devtools "Run/Watch compiled
   debug/profiler" buttons — useful for profiling, not the cause.
2. **GLFW event callbacks** — disabled them to test the "callbacks cause
   jank" theory. *Red herring*, but it led to a real cleanup: the GLFW
   mouse-button hook is now the *authoritative* click-edge source (it never
   misses an edge under a render-capped loop, unlike raylib's poll-to-poll
   edges), with raylib polling kept only as a fallback.
3. **Frame-rate-independent scroll physics** — gated the spring/momentum
   step to a fixed ~60 Hz and clamped the per-frame wheel delta. *Made it
   worse* (desynced physics from the variable render cadence → stutter and
   erratic end-of-list bounce). **Reverted.**
4. **Viewport culling** — `paintEntity` skips the paint (esp. text shaping)
   for entities fully off-screen + margin. *Real win* for the draw cost of
   long lists (only ~visible rows paint), but only "slightly faster"
   overall because the O(n²) lookups and the wake starvation still
   dominated.
5. **Sparse-set ECS** — root cause #1. The big algorithmic fix.
6. **Scroll counts as animating** — recent wheel activity flags the loop as
   animating so it wakes at the display rate during scroll (fixed the
   "hover to get 60 fps" symptom).
7. **Busy-render loop** — root cause #2 and the final fix. Made it fluid.

## Net result

Fluid, near-perfect rendering with only occasional spikes. The combination
that mattered: **sparse set** (frames cheap enough to hit 60+) **× busy
loop** (the loop actually *asks* for frames as fast as possible while
moving). Culling and the mouse-hook cleanup were genuine improvements;
build flags and disabling callbacks were diagnostic dead ends; the scroll-
physics gating was an outright regression that we reverted.

## Practical guidance for new Rae UI work

- **Game / always-animating app:** classic busy loop + `setTargetFPS(n)`
  (or no cap for max fps). Don't use event-wait.
- **Tool / document UI that should idle at ~0% CPU:** use the mobile UI's
  hybrid loop — busy-render while any interaction/transition is active,
  blocking event-wait when idle. Make sure *every* animation source feeds
  the "is animating" flag (a missed source silently starves to the watcher
  poll rate). See `lib/ui/event_loop.rae` and the loop in
  `examples/98_mobile_ui/main.rae`.
- **Keep per-frame work O(n), never O(n²).** Component lookups are O(1)
  (sparse set); avoid nested per-entity scans in any system that runs each
  frame.
- **Don't tune optimization flags to fix a stutter** before confirming the
  cost is constant-factor, not algorithmic or scheduling — `-O` can't fix
  O(n²) or a starved wake loop. Profile first (`Run compiled profiler` in
  devtools builds `-O2 -g` and samples to `/tmp/rae-profile.txt`).
