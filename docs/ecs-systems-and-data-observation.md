# Rae — ECS systems & cross-system data observation — design document

Status: **PROPOSED — converged design, no implementation yet. 2026-07-09.**

The foundational rule this design is built on:

> **Every piece of mutable state has exactly one owner.** Only the
> owning system may mutate its own tables. Everyone else may only
> *observe* (read) or *request* (command). Everything else in this
> document follows from that rule.

From single-ownership everything falls out: observation is read-only;
commands are the only way to request a mutation; systems become easy to
move onto other threads later; and borrowing stays simple because
mutable access never crosses a system boundary.

This supersedes the resource-enum approach in
`examples/106_mobile_ui/data_sources.rae`, rejects the earlier
"two worlds" draft, and rejects a generic runtime `System` base type.
**The only reusable ECS primitive is `Table(T)`.**

---

## 1. Motivating bug (why this matters now)

The 106 mini-player / Now-Playing **cover** does not follow a track
change: play one song, then another, and the cover stays on the first
album. A data change bumps a revision, but **nothing observes it to
update the cover** — the cover is repainted by a per-frame poll walk
(`rebindAlbumCoversToSpotify`) that matches sprites *whose texture key
still equals the browsed album stem*. The persistent dock is mounted
once and keeps its original stem, so after a track change the match
never succeeds. The title updates (it rides the poll's text writes), so
the symptom is "title changes, cover doesn't." The play/pause icon is
correct because it *is* on the revision pattern. The pattern works; the
cover isn't on it — and the pattern should be generic.

---

## 2. `Table(T)` is the primitive; a system is a domain module

The one thing worth making generic is the **table**:

```
Table(T)
  entities
  components (the T values)
  tableRevision      // bumped on add / remove / reorder / sort / filter
  lookup + iteration
```

A **system** is a plain struct of named `Table(T)` fields plus a
revision and the functions over them — a *domain module*, **not** an
instance of a generic ECS runtime:

```rae
struct MediaLibrarySystem {
  revision: Int
  albums:  Table(Album)
  tracks:  Table(Track)
  artists: Table(Artist)
}
```

Conceptual hierarchy — **only `Table` is generic**:

```
Application  contains  Systems      (domain modules)
System       contains  Tables       (plain struct fields)
Table(T)     contains  Components
```

There is no `struct System { tables: ??? }` base: a heterogeneous
`tables` field (`Table(Codec)` vs `Table(Window)` are different types)
would need reflection / type-erasure / `Any` / tuples / language
support — a lot of machinery to avoid writing a few named fields, for a
"base" with almost no shared state. Common operations (bumping the
revision) are ordinary free functions (`markChanged(sys)`), not
inheritance.

- **Split eagerly.** Prefer many small focused systems; split one that
  grows two responsibilities. `PlaybackSystem` is separate from
  `MediaLibrarySystem`; a menu bar is its own `UiMenuBarSystem`, not
  part of a giant `UiSystem`.
- **Integrations that own no tables are functions, not systems.**
  Spotify is integration functions (drive the app via AppleScript, read
  state back) that *write into* the system that owns the data
  (`PlaybackSystem`).

---

## 3. Ownership vs transport (the important separation)

"Who may mutate?" and "how does the request travel?" are **different
questions**, and conflating them is a mistake. Keep the rule
fundamental and the transport an implementation detail.

### 3.1 Ownership (fundamental, fixed)

Only the owning system mutates its own tables. Another system may:

- **observe** its revisions/data (read-only), and
- **send it a command** to request a change.

```
UiPlaybackSystem  observes  PlaybackSystem          // read
Play button clicked  ->  playback.play(trackId)     // command (request)
```

The UI never writes `PlaybackSystem`'s tables; it asks `PlaybackSystem`
to. This is what makes a system safely movable to another thread later.

### 3.2 Transport (an implementation detail, not the model)

*How* `playback.play(trackId)` reaches the owner is hidden behind the
public API. **The API looks synchronous either way:**

```rae
// single-threaded build
func play(sys: mod PlaybackSystem, track: TrackId) { mutate(sys, track) }

// multithreaded build (later) — same signature to callers
func play(sys: mod PlaybackSystem, track: TrackId) { enqueue(Play(track)) }
```

The caller writes `playback.play(track)` and never learns whether that
was a direct mutation or an enqueued message. This gives **one
programming model** — no caller ever asks "should I mutate directly or
enqueue?" — without paying Option-B's cost of routing *every* UI
interaction through event → queue → scheduler → consumer → mutation,
which is miserable to debug (`openExportWindow()` should not require
tracing five hops). We do **not** commit the architecture to async
queues from day one; we commit to single-ownership, and let transport
change under a stable API when/if threads arrive.

---

## 4. Three system *layers* (different lifetimes)

Each layer is a plain struct (of tables); each has a distinct lifetime.

### 4.1 Application-state systems (owned data)

```
MediaLibrarySystem — albums, tracks, artists, thumbnails
PlaybackSystem     — nowPlaying (current track / art / position / isPlaying)
ExportVideoSystem  — codecs, presets, outputFormats, destinations
```

Mutated only by their own domain logic / integration functions.

### 4.2 Persistent UI systems (widget state)

Split per feature — no single `UiSystem`:

```
UiPlaybackSystem    — AlbumImage, TrackTitle, PlayButton
UiExportVideoSystem — ExportWindow, BitrateDropdown, CodecList, ProgressBar
UiMenuBarSystem     — the menu bar and its items
```

Persistent widget state (widget tree, focus, selection, expanded nodes,
scroll position). They observe data systems and command them.

### 4.3 The render system (transient, cache-oriented, dependency-driven)

Rendering state has a different lifetime from widget state, so it lives
in its own system — but it is **not** thrown away every frame, and it
is **not** event-driven. It is **dependency-driven**, like a browser /
modern retained-mode GUI:

```
ComputedRect  depends on  Window.size, Parent.layout, Button.text, Theme.padding
```

- Each cached entry records the revisions of the inputs it depends on.
- When a dependency's revision changes → mark the entry **dirty**.
- Next render pass: recompute dirty entries, **reuse** the rest.

`ComputedRect`s live in a reusable **pool** (a rect can survive even
when the entity it targets elsewhere changes, as long as nothing *that
rect depends on* changed). Glyph layout, GPU buffers, and
virtualization state reuse the same way. List **virtualization** lives
here too: only visible rows get render entities, so list cost is
O(visible), not O(total). No command/event indirection — just "did my
inputs change?".

### 4.4 The observation chain

```
MediaLibrarySystem / PlaybackSystem   application state
   ↑ observes / commands
UiPlaybackSystem                      persistent widgets
   ↑ dependency-driven cache
UiRenderSystem                        transient render state
```

---

## 5. Three revision levels (eagerly bumped)

Every observable change is one of three monotonic counters, **bumped
eagerly at the write site** so reads are free (no table scans on read):

- **Component-instance revision** — a successful field write bumps the
  instance's `revision`. No per-field versions. Granularity is the
  **component**, not the entity.
- **Table revision** — `Table(T).tableRevision`, bumped on structural
  change; lives in the primitive, so every system gets it free.
- **System revision** — the plain `revision: Int` on the struct, bumped
  when any member changes. A write bumps component → (table if
  structural) → system in one go, so a page observing the system reads
  one `Int`.

**Explicitly rejected:**

- **Entity-wide revision** — too coarse; a `Waveform` widget must not
  redraw because `Permissions` on the same entity changed.
- **Implicit / compiler-driven bumping** (e.g. on `mod` access) — a
  `componentMod` borrow doesn't imply a change (the write may be gated
  by an `if`), so it would over-invalidate and obscure intent. Keep it
  **explicit**: `codec.bitRate = x; bumpRevision(codec)`, or wrap in
  helper mutation functions that bump only after a successful change.

**Ladder (coarse → fine):** system → table → component instance.

---

## 6. Observation is system-to-system, by ID, read-only

> **A system may observe entities or tables in another system.**

Handles are **IDs, not pointers.** A `SystemRegistry` owns all systems;
a `SystemId` indexes it. Crucially, the **registry never hands out
`mod`** to outsiders — each system exposes only a `view` (read) and its
`commands` (request). So another system can do

```rae
playback.view.currentTrack     // read
playback.play(track)           // command
```

but **never** `playback.currentTrack = ...`. That is single-ownership
(§3) enforced by the access surface, and it makes borrowing trivial:
mutable access never crosses a system boundary, so a refresh that reads
many systems only ever holds `view`s.

An `Observe` stores ids + the observed revision (no pointers — which
also makes serialization and hot-reload easy):

```
Observe {
  system:  SystemId
  kind:    component | table | system
  target:  (tableId, entityId) | tableId | (whole system)
  observedRevision: Int
}
```

Refresh is the same three lines for every kind and every pair of
systems:

```
current := revisionOf(registry, observe)   // resolves SystemId, reads via view
if current != observe.observedRevision {
    rebuild the observing entity from the observed data
    observe.observedRevision = current
}
```

Because targets are `(system, table, entity)` ids, the mechanism is
fully generic — media library, playback, filesystem, compiler state,
undo, AI generation are all observed the same way, with **no**
per-feature `Resource` enum. `DataDependency { resource, field,
observedRevision }` today is the same shape with an *enum* target; the
migration swaps the enum for ids.

---

## 7. Worked shape (the motivating fix)

```
// UiPlaybackSystem: cover observes PlaybackSystem's nowPlaying instance
observe(cover, system: PlaybackId, component: (nowPlayingTable, entity))

if rev(registry, cover.observe) != cover.observed {   // instance revision
    cover.artKey = playback.view.nowPlaying(entity).artKey
    cover.observed = current
}
```

Spotify's integration functions write the new track/art into
`PlaybackSystem` (via its own command/API, bumping the instance
revision); the cover observes that instance by id and re-points. No
per-frame sprite walk, no stale stem. A "Reset presets" button issues
`exportVideo.resetPresets()` (command); the owner mutates and bumps, the
page rebuilds via observation next pass.

---

## 8. Migration from the current 106 pattern

Non-breaking, in slices (mirroring the theme-system migration style):

- **D0 — `Table(T)` + eager revisions.** Add `tableRevision`, component
  `revision`, system `revision` + `markChanged`. Leave `AppState`/
  `UiWorld` intact.
- **D1 — `SystemRegistry` (`view` + `commands`, no `mod` out) + generic
  `Observe` + one `refreshObservers(registry)`** replacing the
  per-domain `refreshXViewsIfNeeded`. Old path stays in parallel.
- **D2 — carve out systems, split eagerly.** `MediaLibrarySystem` /
  `PlaybackSystem` / …; Spotify → integration functions commanding
  `PlaybackSystem`. Split UI into per-feature UI systems + a
  dependency-driven `UiRenderSystem`. UI→data writes go through commands.
- **D3 — port consumers.** Convert `DataDependency(enum)` →
  `Observe(SystemId + ids)`: play/pause icon and **cover → observe
  `PlaybackSystem.nowPlaying` (fixes the motivating bug)**; codec list →
  observe the `codecs` table. Delete `rebindAlbumCoversToSpotify` + the
  poll's UI writes.
- **D4 — delete the enums** (`DataResourceId`/`DataFieldId`/
  `AppDataResources`/`bumpXResource`).

Each slice keeps the previous mechanism until the next removes it; 106
stays runnable throughout.

---

## 9. Remaining open questions

Most are resolved in the body (single-ownership; split eagerly; IDs +
registry with `view`+`commands` only; eager bumping; render cache is
transient + dependency-driven; explicit not implicit bumping;
synchronous-looking command API with transport hidden). Still open:

1. **Command implementation, per system.** Direct function call (the
   default, single-threaded) vs an internal queue (later, per system
   that moves off-thread). The *API* is fixed synchronous; only the
   body changes. Decide per system if/when threading arrives.
2. **`SystemRegistry` access phasing.** Even with `view`+`commands`
   only, confirm the frame phase order so a refresh (many `view`s) and
   command application (`mod` inside one owner) never overlap on the
   same system — likely a read/observe phase then a command/apply phase.
3. **Render-cache dependency tracking granularity.** How precisely a
   `ComputedRect` records "depends on Window.size, Theme.padding, …" —
   an explicit small dependency list per cache entry vs observing the
   revisions of a fixed set of layout inputs.
4. **Future language support (leave open).** Automatic table
   registration, compile-time reflection, codegen/serialization, or a
   `system Foo { ... }` sugar generating the boilerplate. Useful
   eventually; the architecture must work today with plain structs +
   `Table(T)` and must not depend on features that don't exist.

---

## 10. Summary

1. **Single-ownership is the foundation.** Every piece of mutable state
   has exactly one owner; only the owner mutates its tables. Observation
   is read-only; commands request mutations.
2. **Ownership ≠ transport.** The command API is synchronous-looking;
   whether it mutates directly (today) or enqueues (later, per system)
   is hidden. One programming model, no queues forced everywhere.
3. **`Table(T)` is the only generic primitive.** A "system" is a domain
   module: a plain struct of `Table(T)` + `revision` + functions. Split
   eagerly; table-less integrations (Spotify) are just functions.
4. **Three layers** by lifetime: application-state, persistent
   per-feature UI, and a transient **dependency-driven** `UiRenderSystem`
   (pooled rects, glyph/GPU reuse, virtualization).
5. **Three eagerly-bumped revision levels** — component / table / system.
   No per-field, no entity-wide, no implicit bumping.
6. **References are IDs**; the registry exposes only `view` + `commands`,
   never `mod` — so mutable access never crosses a system boundary.
