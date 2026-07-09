# Rae — ECS systems, ownership & data observation — design document

Status: **PROPOSED — converged. Foundational design accepted; remaining
items are engineering tradeoffs. No implementation yet. 2026-07-09.**

This is not "an ECS UI." It is an **application architecture where every
subsystem is an ECS domain with strict ownership** — the same shape for
UI, playback, media library, export, compiler workers, asset loading,
networking, undo. The foundational rule:

> **Every piece of mutable state has exactly one owner, and a system
> only ever changes its own state while processing its own commands.**
> Everyone else may only *observe* (read) or *send a command*.

That single invariant is what everything below follows from, and it is
the kind of simple invariant that ages well as software grows from an
iPhone app to Logic-Pro-class software or a game.

The only reusable ECS primitive is `Table(T)`. This supersedes the
resource-enum approach in `examples/106_mobile_ui/data_sources.rae` and
rejects both the "two worlds" draft and a generic runtime `System` base.

---

## 1. Motivating bug (why this matters now)

The 106 mini-player / Now-Playing **cover** doesn't follow a track
change: play one song, then another, and the cover stays on the first
album. A data change bumps a revision, but **nothing observes it to
update the cover** — the cover is repainted by a per-frame poll walk
that matches sprites *whose texture key still equals the browsed album
stem*. The persistent dock keeps its mount-time stem, so after a track
change the match never succeeds. The title updates (it rides the poll's
text writes), so the symptom is "title changes, cover doesn't." The
play/pause icon is correct because it *is* on the revision pattern. The
pattern works; the cover isn't on it — and it should be generic.

---

## 2. `Table(T)` is the primitive; a system is a domain module

```
Table(T)
  entities
  components (the T values)
  tableRevision      // bumped on add / remove / reorder / sort / filter
  lookup + iteration
```

A **system** is a plain struct of named `Table(T)` fields + a revision +
functions — a *domain module*, not an instance of a generic ECS
runtime:

```rae
struct MediaLibrarySystem {
  revision: Int
  albums:  Table(Album)
  tracks:  Table(Track)
  artists: Table(Artist)
}
```

Hierarchy — **only `Table` is generic**:

```
Application  contains  Systems      (domain modules / actors)
System       contains  Tables       (plain struct fields)
Table(T)     contains  Components
```

There is no `struct System { tables: ??? }` base (heterogeneous tables
would demand reflection/type-erasure/`Any`/tuples for almost no shared
state). Shared operations are free functions (`markChanged(sys)`), not
inheritance.

- **Split eagerly** into small focused systems. `PlaybackSystem` is
  separate from `MediaLibrarySystem`; a menu bar is its own
  `UiMenuBarSystem`, not part of a giant `UiSystem`.
- **Integrations that own no tables are functions, not systems** —
  Spotify is integration functions (drive the app via AppleScript, read
  state back) that *command* the system that owns the data
  (`PlaybackSystem`).

---

## 3. Systems are actors

Each system already has owned state, public commands, read-only
observation, and revisions — that is essentially an actor. Make it the
standard shape from day one:

```rae
struct PlaybackSystem {
  revision: Int
  inbox:    Queue(PlaybackCommand)
  nowPlaying: Table(NowPlaying)
  // ...
}
```

The rule is uniform and has **no exceptions**:

> A system mutates its own tables **only** while draining its own inbox.
> No code ever writes another system's tables directly.

To request a change you send a command; the owning system applies it
during the command phase (§4). Single-threaded, "send" appends to the
inbox and it drains this frame; if a system later moves to another
thread, "send" crosses a channel and it drains there — **same shape,
transport hidden.** There is exactly one mutation mechanism, so no one
ever asks "mutate directly or enqueue?".

**The tradeoff, stated honestly.** Command application adds ~one frame
of latency and some boilerplate (a command type + an apply arm per
mutation), and debugging a mutation means looking at the command
phase rather than a direct call. We accept it because the payoff is
large and compounding: deterministic execution, replay, undo,
networking, and multithreading all become natural, and the "only touch
my own state, only in my command phase" invariant stays true no matter
how big the app gets. The frame-phase pipeline (§4) keeps commands
inspectable in **one** place, so this is not the "five random hops"
indirection of ad-hoc event buses.

---

## 4. Frame phases (this dissolves borrowing)

Define the engine phases up front; ownership + `view`/`mod` access fall
out of the phase, so there is never a borrowing question:

```
1. Input                — gather OS/pointer/key events
2. Command collection   — everyone SENDS commands (append to inboxes)
3. Command application   — each system drains its own inbox; it alone holds `mod self`
4. Observation refresh   — everyone reads with `view`; rebuild widgets whose observed revision changed
5. Layout                — resolve rects (retained cache, §5.3)
6. Render                — emit draw commands
```

- In **command application**, exactly one system at a time holds
  `mod self`; nothing else is mutating, so no aliasing.
- In **observation refresh** and **layout/render**, everyone holds
  `view` only.

Because `mod` access is confined to phase 3 (a system on its own
tables) and every other phase is read-only, mutable access **never
crosses a system boundary** and the borrow story is trivial.

---

## 5. Three system *layers* (different lifetimes)

### 5.1 Application-state systems (owned data)

```
MediaLibrarySystem — albums, tracks, artists, thumbnails
PlaybackSystem     — nowPlaying (current track / art / position / isPlaying)
ExportVideoSystem  — codecs, presets, outputFormats, destinations
```

### 5.2 Persistent UI systems (widget state)

Split per feature — no single `UiSystem`:

```
UiPlaybackSystem    — AlbumImage, TrackTitle, PlayButton
UiExportVideoSystem — ExportWindow, BitrateDropdown, CodecList, ProgressBar
UiMenuBarSystem     — the menu bar and its items
```

Persistent widget state (tree, focus, selection, expanded nodes, scroll
position). They observe data systems and command them.

### 5.3 The retained render cache (transient, dependency-driven)

Rendering state lives in its own system, but it is **not** rebuilt every
frame and **not** event-driven — it is a **retained render cache**
(as in browsers / SwiftUI / AppKit), dependency-driven:

```
ComputedRect  depends on  Window.size, Parent.layout, Button.text, Theme.padding
```

Each cache entry records the revisions of its inputs; when a
dependency's revision changes the entry is marked **dirty**; the next
layout/render pass recomputes dirty entries and **reuses** the rest.
`ComputedRect`s live in a reusable pool; glyph layout, GPU buffers, and
virtualization state reuse the same way. List **virtualization** lives
here — only visible rows get render entities, so list cost is
O(visible), not O(total).

---

## 6. System lifecycle (where timers, animation, polling, async live)

Observation and commands are not the whole story — a system also has
**internal behaviour over time**. Give every system an explicit
lifecycle:

```rae
initialize(sys)      // allocate tables, register with the SystemRegistry
processCommands(sys) // phase 3: drain inbox, mutate own tables, bump revisions
update(sys, dt)      // internal behaviour: advance timers/animations,
                     // sample pollers (Spotify), harvest completed async work
shutdown(sys)        // release resources
```

`update(sys, dt)` is where things that are neither observations nor
external commands live:

- **Timers / animations** — advance internal state each frame.
- **Polling an external source** (Spotify-over-AppleScript, a watched
  folder) — sample on an interval and write the result into the
  system's own tables.
- **Async completion** — a finished file/network/AI job hands its result
  to the owning system, which folds it into its tables during `update`.

All of these are the system mutating **its own** state (so they bump its
own revisions and observers react) — they are internal behaviour, not a
cross-system write. `update` runs in the command-application phase (or
immediately after it) so it, too, is the only-my-own-tables writer.

---

## 7. Three revision levels (eagerly bumped)

Bumped **eagerly at the write site** so reads are free:

- **Component-instance revision** — a successful field write bumps the
  instance `revision`. No per-field versions. Granularity is the
  **component**, not the entity.
- **Table revision** — `Table(T).tableRevision`, bumped on structural
  change; lives in the primitive.
- **System revision** — the plain `revision: Int`, bumped when any
  member changes (component → table-if-structural → system in one go).

**Rejected:** entity-wide revision (too coarse — a `Waveform` widget
must not redraw because `Permissions` on the same entity changed); and
implicit/compiler-driven bumping (a `mod` borrow doesn't imply a change;
keep it explicit — `bumpRevision(codec)` or bump-after-success helpers).

---

## 8. Observation is system-to-system, by ID, read-only

> **A system may observe entities or tables in another system.**

Handles are **IDs, not pointers.** A `SystemRegistry` owns the systems;
a `SystemId` indexes it. The registry hands out only a **`view`** and a
system's **commands** — never `mod` — so ownership is enforced by the
access surface:

```rae
playback.view.currentTrack   // read
playback.play(track)         // command (a send)
// playback.currentTrack = ...   // impossible — no mod is exposed
```

An `Observe` stores ids + the observed revision (no pointers — easy to
serialize / hot-reload):

```
Observe { system: SystemId, kind: component|table|system,
          target: (tableId,entityId)|tableId|whole, observedRevision: Int }
```

Refresh (phase 4) is the same three lines everywhere:

```
current := revisionOf(registry, observe)      // resolve SystemId, read via view
if current != observe.observedRevision {
    rebuild the observing entity from the observed data
    observe.observedRevision = current
}
```

Targets are `(system, table, entity)` ids, so the mechanism is fully
generic — playback, media library, filesystem, compiler, undo, AI are
all observed identically, with **no** per-feature `Resource`/`Field`
enum. Retiring `DataResourceId`/`DataFieldId` in favour of real
system/table/entity ids is what makes this feel like a real ECS rather
than a bespoke reactive layer.

---

## 9. Worked shape (the motivating fix)

```
// UiPlaybackSystem: cover observes PlaybackSystem's nowPlaying instance
observe(cover, system: PlaybackId, component: (nowPlayingTable, entity))

// phase 4:
if rev(registry, cover.observe) != cover.observed {
    cover.artKey = playback.view.nowPlaying(entity).artKey
    cover.observed = current
}
```

Spotify's integration functions `update()` PlaybackSystem (sample the
app, write `nowPlaying`, bump its instance revision — all its own
tables); the cover observes that instance by id and re-points. No
per-frame sprite walk, no stale stem.

---

## 10. Migration from the current 106 pattern

Non-breaking, in slices; 106 stays runnable throughout:

- **D0 — `Table(T)` + eager revisions** (table/component/system), leave
  `AppState`/`UiWorld` intact.
- **D1 — `SystemRegistry` (`view`+`commands`, no `mod` out) + generic
  `Observe` + a `refreshObservers(registry)`** replacing per-domain
  refreshers; old path in parallel.
- **D2 — carve out systems, split eagerly**, give each an `inbox` +
  lifecycle; Spotify → integration functions in `PlaybackSystem.update`.
  Introduce the frame phases (§4) and the retained render cache.
- **D3 — port consumers** to `Observe(SystemId + ids)` and commands:
  play/pause icon and **cover → observe `PlaybackSystem.nowPlaying`
  (fixes the motivating bug)**; codec list → observe the `codecs` table.
  Delete the poll walk + the poll's UI writes.
- **D4 — delete the enums** (`DataResourceId`/`DataFieldId`/
  `AppDataResources`/`bumpXResource`).

---

## 11. Remaining open questions (engineering tradeoffs)

1. **Command latency mitigation.** ~one-frame latency is usually fine
   for UI; for input-echo cases that must feel instant, allow a system
   to apply a command immediately within its own phase, or run
   command-application before observation in the same frame (§4 already
   does the latter). Measure before optimizing.
2. **Command/inbox ergonomics.** Boilerplate per command (a variant +
   an apply arm). A future `system`/`command` sugar (§ below) could
   generate it; until then keep it explicit and small.
3. **Per-system threading.** Which systems actually move off-thread
   (asset loading, compiler workers, AI), and how the inbox becomes a
   cross-thread channel without changing callers.
4. **Retained-cache dependency granularity.** How precisely a cache
   entry records "depends on Window.size, Theme.padding, …" — an
   explicit per-entry dependency list vs observing a fixed set of layout
   inputs' revisions.
5. **Future language support (leave open).** Automatic table
   registration, compile-time reflection, codegen/serialization, or a
   `system Foo { ... }` / `command` sugar generating inboxes + apply
   arms + tables + revisions. Useful eventually; the architecture must
   work today with plain structs + `Table(T)` and must not depend on it.

---

## 12. Summary

1. **Single-ownership + actor rule:** every mutable state has one owner;
   a system changes its own tables **only** while draining its own
   inbox. Observe (read) or command (request) — nothing else.
2. **`Table(T)` is the only generic primitive.** A "system" is a domain
   module (plain struct of tables + revision + functions + inbox +
   lifecycle). Split eagerly; table-less integrations are just functions.
3. **Explicit frame phases** (input → collect → apply → observe →
   layout → render) confine `mod` to a system's own command phase, so
   mutable access never crosses a boundary and borrowing is trivial.
4. **System lifecycle** (`initialize`/`processCommands`/`update`/
   `shutdown`): timers, animation, polling, and async completion are
   internal behaviour in `update`, mutating only the system's own tables.
5. **Three layers** by lifetime: application-state, persistent per-feature
   UI, and a **retained render cache** (dependency-driven, pooled,
   virtualized).
6. **Three eagerly-bumped revision levels** — component / table / system.
   No per-field, no entity-wide, no implicit bumping.
7. **References are IDs**; the registry exposes only `view` + `commands`.
