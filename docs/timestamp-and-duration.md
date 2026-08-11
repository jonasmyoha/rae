# Timestamp and Duration

Two ordinary Rae types in `lib/time`:

```rae
type Timestamp pub {
  seconds: Float64
}

type Duration pub {
  seconds: Float64
}
```

That is the whole mechanism. They are different types for the ordinary reason
that they are different `type` declarations — no compiler special cases, no
nominal scalar kinds, no operator overloading, no new language machinery. Either
can gain fields later (a clock id, a monotonic flag, a calendar) without any of
this changing.

```rae
import time

var lastPoll: Timestamp = time.never()
let interval: Duration = time.seconds(n: 1.0)

let now: Timestamp = time.now()
if lastPoll.hasElapsed(now: now, interval: interval) {
  lastPoll = now
  ...
}
```

## Why

A point in time and a span of time are both "seconds as a `Float64`", so the
compiler had no way to tell a reading from an interval — and neither does a
reader skimming a diff. That is not academic. Example 106 stored a wall-clock
reading (`uiNowSeconds()`, ~1.75e9 epoch seconds) in fields meant to hold
intervals. An f32 at epoch magnitude has a **128-second ulp**, so every stored
timestamp snapped to a multiple of 128 and each "has enough time passed?" test
collapsed into a constant — the app either polled every frame or never polled,
depending on where the clock sat inside the current 128 s bucket. Same binary,
different behaviour by time of day (#407).

Widening those fields to `Float64` removed the symptom. It did not remove the
confusion that produced it: the code still typed a reading and an interval
identically, so the next such mix-up would be just as invisible.

## Operations are ordinary functions

Each takes `this` as its first parameter, so the existing dot-call syntax
applies:

| call | result |
|---|---|
| `later.since(earlier: base)` | `Duration` |
| `t.advance(by: d)`, `t.rewind(by: d)` | `Timestamp` |
| `t.isBefore(other:)`, `t.isAfter(other:)`, `t.isNever()` | `Bool` |
| `last.hasElapsed(now:, interval:)` | `Bool` — the throttle predicate |
| `d.plus(other:)`, `d.minus(other:)`, `d.scaled(by:)` | `Duration` |
| `d.ratio(to:)` | `Float64` — a ratio is a plain number |
| `d.isLongerThan(other:)`, `d.isShorterThan(other:)` | `Bool` |
| `t.epochSeconds()`, `d.inSeconds()`, `d.inMillis()` | `Float64` |

Constructors: `time.now()`, `time.atSeconds(s:)`, `time.seconds(n:)`,
`time.millis(n:)`, `time.never()`, `time.zero()`.

There is **no operator overloading**. `now - last` does not compile, because
`-` is not defined for these types — the same reason it is not defined for any
other struct. `now.since(earlier: last)` says which of the two possible
subtractions was meant, which is the point: `Timestamp - Timestamp` is a span
while `Timestamp - Duration` is another instant, and a single `-` cannot mean
both.

Comparison also goes through named functions (`isBefore`, `isLongerThan`)
because `<` does not apply to structs. Nothing special is happening here —
these read the `seconds` field and compare two `Float64`s.

`seconds` is an ordinary public field, so `t.seconds` works directly. The
accessors exist so call sites read as intent rather than as field poking, not to
hide anything.

## Zero cost

A single-field struct lowers to exactly that:

```c
struct rae_Timestamp {
  double seconds;
};
```

No wrapper object, no allocation, no indirection — the same double, passed by
value.

## What catches the mix-up, and where

Assigning a `Duration` where a `Timestamp` belongs is rejected, which is the
whole point. But be aware **where** the rejection comes from today: Rae's
semantic analysis does not check struct assignment compatibility at all, so the
error surfaces from the **C compiler**, against generated code:

```
out.c:2728:5: error: assigning to 'rae_Timestamp' (aka 'struct rae_Timestamp')
              from incompatible type 'rae_Duration'
```

The mistake is caught — nothing unsound gets through — but the diagnostic points
at a `.c` line rather than the offending `.rae` line. That is a pre-existing gap
affecting *every* user-defined type, not something specific to these two, and it
is the same family as the unchecked call arguments below. Filed as **#414**;
fixing it improves diagnostics for all Rae types at once, which is a much better
deal than special-casing time.

## Known gap

**Call arguments are not conversion-checked** either. Passing a `Timestamp` to a
`Duration` parameter is not caught by sema; as above, the C compiler catches the
struct case. This is long-standing and not time-specific — passing a `Float` to
a `Float64` parameter is equally unchecked. Tracked as **#410**.

## Adoption

Example 106's Spotify poll — the field the bug lived in — is migrated:
`SpotifyState.lastPoll` is a `Timestamp`, the interval is a `Duration`, and the
throttle is `lastPoll.hasElapsed(now:, interval:)`. Idle behaviour measured
headless with Spotify on, 10 seconds: **213 loop iterations, 26 repaints**, from
~750 iterations with every frame repainting before the #407 fixes.

Deliberately NOT done yet, and queued as **#413** rather than rushed:

- The remaining 106 time fields (`PlaybackState.prevTime`, `HeroAnim.startTime`,
  `FrameRenderResult.lastRenderTime`, `PlayHistoryEntry.playedAt`). Each is
  small, but they thread through the frame pipeline's signatures.
  `playedAt` also serialises to JSON, so it wants a deliberate format decision.
- Flipping `uiNowSeconds()` / `gpu2d.nowSeconds()` to return `Timestamp`: 34 call
  sites across 13 files, every one of which does arithmetic that would become
  `time.*` calls. Worth doing as its own reviewable sweep, not alongside the
  types landing.

The older `Timer` type in the same module (Int nanoseconds, stopwatch API) is
unrelated and untouched.
