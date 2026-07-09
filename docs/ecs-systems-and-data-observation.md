# Rae — ECS systems & cross-system data observation — design document

Status: **PROPOSED (design only, no implementation). 2026-07-09.**

The guiding principle this document adopts as the test for every
decision below:

> An application is a collection of small, cohesive **systems**. A
> system is a plain struct of `Table(T)` fields plus the functions that
> operate on them — a *domain module*, not an instance of some generic
> ECS runtime. Systems communicate by **one-way observation** (read a
> revision, rebuild on change) and **commands the other way** (ask the
> owning system to mutate its own tables). No system writes another's
> tables.

This supersedes the resource-enum approach in
`examples/106_mobile_ui/data_sources.rae`, rejects the earlier
"two worlds" (`UiWorld` + `DataWorld`) draft, and rejects a generic
runtime `System` base type. **The only reusable ECS primitive is
`Table(T)`.**

---

## 1. Motivating bug (why this matters now)

The 106 mini-player / Now-Playing **cover** does not follow a track
change: play one song, then another, and the cover stays on the first
album. A data change bumps a revision (`bumpSpotifyResource`), but
**nothing observes it to update the cover** — the cover is repainted by
a per-frame poll walk (`rebindAlbumCoversToSpotify`) that matches
sprites *whose texture key still equals the browsed album stem*. The
persistent dock is mounted once and keeps its original stem, so after a
track change the match never succeeds. The title updates (it rides the
poll's text writes), so the symptom is "title changes, cover doesn't."

The play/pause icon is correct because it *is* on the revision pattern.
The pattern works; the cover isn't on it — and the pattern should be
generic, not hand-wired per data domain.

---

## 2. `Table(T)` is the primitive; a system is a domain module

The one thing worth making generic is the **table**. `Table(T)`
naturally owns:

```
Table(T)
  entities
  components (the T values)
  tableRevision      // bumped on add / remove / reorder / sort / filter
  lookup + iteration
```

A **system** is then just a normal struct with named `Table(T)` fields
and a revision — no inheritance, no base type, no hidden machinery:

```rae
struct MediaLibrarySystem {
  revision: Int
  albums:  Table(Album)
  tracks:  Table(Track)
  artists: Table(Artist)
}

struct ExportVideoSystem {
  revision: Int
  codecs:       Table(Codec)
  presets:      Table(Preset)
  destinations: Table(Destination)
}
```

Conceptual hierarchy — **only `Table` is generic**:

```
Application  contains  Systems      (domain modules)
System       contains  Tables       (plain struct fields)
Table(T)     contains  Components
```

### 2.1 "System" means a domain module — nothing more

A `PlaybackSystem` is **not** "an instance of a generic ECS runtime."
It is a struct that owns some `Table(T)`s and the functions over them.
There is exactly one meaning of "system" in this design: a
domain/feature module. The ECS-container role is filled entirely by
`Table(T)`. This is why there is no `struct System { tables: ??? }`
base — a heterogeneous `tables` field (`Table(Codec)` vs `Table(Window)`
are different types) would need reflection / type-erasure / `Any` /
tuples / language support, a large amount of machinery to avoid writing
a few named fields, for a "base" with almost no shared state.

### 2.2 Split eagerly; small systems win

Prefer many small systems over a few large ones. A system with two
responsibilities should be split. Small systems are easier to
understand, test, and maintain; some will still be large, but a small
focused system is the default. So `PlaybackSystem` is separate from
`MediaLibrarySystem`; a menu bar is its **own** `UiMenuBarSystem`, not a
member of one giant `UiSystem`.

### 2.3 Shared behaviour is free functions

Common operations (bumping the revision) are ordinary functions, not
inheritance:

```rae
func markChanged(sys: mod MediaLibrarySystem) {
  sys.revision = sys.revision + 1
}
```

### 2.4 Integrations are functions, not systems

If a concern owns no tables it is not a system. **Spotify is a set of
integration functions** (control the app via AppleScript, read state
back) that *write into* the system that owns the data. The album/track
catalog lives in `MediaLibrarySystem`; "what's playing" lives in a
separate `PlaybackSystem`; Spotify's functions write `PlaybackSystem`.

---

## 3. Communication: observe one way, command the other

Two systems relate in exactly two ways, and keeping them separate is
what preserves ownership:

- **Observation (read, pull):** a system reads another system's
  revisions and rebuilds on change (§5). One-way and read-only.
- **Commands (write, push):** when a system needs to *change* another
  system's data, it does **not** touch that system's tables. It calls
  the owning system's public function (or enqueues a command/event);
  the owning system mutates its own tables and bumps its own revisions.

```
UiPlaybackSystem  observes  PlaybackSystem          // read
Play button clicked  ->  PlaybackSystem.play()      // command
```

So the UI never writes `PlaybackSystem`'s tables; it asks
`PlaybackSystem` to. This is the standard ECS answer to "system A needs
to modify system B": route the write through B's API / a command queue,
keeping B the sole mutator of its tables. The dependency (A knows B's
command API) is real but explicit and one-directional per channel
(A observes B's state; A commands B's API), which avoids tangled
two-way table access.

---

## 4. Three system *layers* (different lifetimes)

Each layer is a plain struct (of tables); each has a distinct lifetime.

### 4.1 Application-state systems (owned data)

```
MediaLibrarySystem — albums, tracks, artists, thumbnails
PlaybackSystem     — nowPlaying (current track / art / position / isPlaying)
ExportVideoSystem  — codecs, presets, outputFormats, destinations
```

Mutated only by their own domain logic / integration functions. They
know nothing about the UI.

### 4.2 Persistent UI systems (widget state)

Split per feature — no single `UiSystem`:

```
UiPlaybackSystem   — AlbumImage, TrackTitle, PlayButton
UiExportVideoSystem— ExportWindow, BitrateDropdown, CodecList, ProgressBar
UiMenuBarSystem    — the menu bar and its items
```

Persistent widget state: the widget tree, focus, selection, expanded
tree nodes, scroll position. They observe data systems and command them.

### 4.3 The render system (transient + cache-oriented)

UI *state* and UI *rendering* are different data with different
lifetimes. The render system is **not** "thrown away every frame" —
that would be wasteful. It is **transient and cache-oriented**: the
place rendering-related state lives, where some entries are recreated
and others reused when nothing affecting them changed.

```
UiRenderSystem
  computedRects (a reusable POOL), clipRects, layoutCache,
  glyphLayout / glyphBatches, virtualizationState,
  drawCommands, gpuBuffers / textureAtlasRefs
```

- `ComputedRect`s live in a **pool** and are reused across frames; a
  rect can survive even when the entity it targets in another system
  changes, as long as nothing affecting *that rect* changed.
- Glyph layout, GPU buffers, and virtualization state are likewise
  reused until invalidated.
- List **virtualization** lives here: only visible rows get render
  entities, so list render cost is O(visible), not O(total).

### 4.4 The observation chain

```
MediaLibrarySystem / PlaybackSystem   application state
   ↑ observes
UiPlaybackSystem                      persistent widgets
   ↑ observes
UiRenderSystem                        transient / cached render state
```

---

## 5. Three revision levels (eagerly bumped)

Every observable change is one of three monotonic counters, **bumped
eagerly at the write site** so reads are free (no table scans on read):

### 5.1 Component-instance revision — "this object's data changed"

Each component instance carries a `revision`; a successful field write
bumps it. **No per-field versions** (one counter per instance).
Granularity is the **component**, not the entity (see 5.4).

### 5.2 Table revision — "objects added / removed / reordered"

`Table(T).tableRevision`, bumped on structural change. Lives in the
`Table(T)` primitive, so every system gets it for free. A list watches
the table; a row watches its own instance.

### 5.3 System revision — "something in this subsystem changed"

The plain `revision: Int` on the system struct, bumped whenever a member
table/instance changes. Eager: a write bumps component → (table if
structural) → system in one go, so a page that observes the system
reads one `Int`.

### 5.4 Explicitly rejected

- **Entity-wide revision** — too coarse; a `Waveform` widget must not
  redraw because a `Permissions` component on the same entity changed.
  It observes the `Waveform` component, not the entity.
- **Implicit / compiler-driven bumping** — e.g. bumping on `mod`
  component access. Rejected: a `componentMod` borrow does not imply a
  change (the mutation may be gated by an `if`), so implicit bumping
  would over-invalidate and be harder to reason about. Keep it
  **explicit**: `codec.bitRate = x; bumpRevision(codec)`, or wrap it in
  helper mutation functions that bump only after a successful change.
  Explicit is easier to understand and debug.

**Ladder (coarse → fine):** system `revision` → `Table.tableRevision` →
component instance `revision`.

---

## 6. Observation is system-to-system, by ID

> **A system may observe entities or tables in another system.**

Handles are **IDs, not pointers.** A `SystemRegistry` owns all systems;
a `SystemId` indexes it:

```
SystemId  ->  SystemRegistry  ->  { PlaybackSystem, MediaLibrarySystem, ... }
```

An `Observe` therefore stores only ids + the observed revision — no
pointers, which also makes serialization and hot-reload straightforward:

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
current := revisionOf(registry, observe)   // resolves SystemId, reads the counter
if current != observe.observedRevision {
    rebuild the observing entity from the observed data
    observe.observedRevision = current
}
```

Because targets are `(system, table, entity)` ids, the mechanism is
fully generic: media library, playback, filesystem, compiler state,
undo, AI generation — all observed the same way. There is **no**
`SpotifyResource`/`LibraryResource` enum per feature; adding a data
source is "add a system struct with `Table(T)` fields," and the
observation layer needs no new cases.

### 6.1 Relationship to today's `DataDependency`

`DataDependency { resource: DataResourceId, field: DataFieldId,
observedRevision }` is the same shape with an **enum** target. The
migration (§8): replace `(resource, field)` with `(SystemId, kind,
target ids)`, keep `observedRevision`, and replace the eight
`bumpXResource` + per-domain refreshers with one generic revision read +
one generic refresh walk over the registry.

---

## 7. Worked shapes

### 7.1 Leaf widget (the cover — the motivating fix)

```
// UiPlaybackSystem: cover observes PlaybackSystem's nowPlaying instance
observe(cover, system: PlaybackId, component: (nowPlayingTable, entity))

if rev(registry, cover.observe) != cover.observed {   // instance revision
    cover.artKey = artKeyOf(PlaybackSystem, entity)
    cover.observed = current
}
```

Spotify's integration functions write the new track/art into
`PlaybackSystem.nowPlaying` (bumping its instance revision); the cover
observes that instance by id and re-points. No per-frame sprite walk, no
stale stem.

### 7.2 List + virtualization (two layers)

- `CodecList` (`UiExportVideoSystem`) observes the `codecs` **table** in
  `ExportVideoSystem`; on `tableRevision` change it rebuilds its rows.
- `UiRenderSystem` reuses pooled `ComputedRect`s for unchanged rows and
  materializes **only visible** rows into transient draw entities. A
  field edit inside one codec bumps that row's *instance* revision (row
  rebuilds), not the list's table revision.

### 7.3 Page + command

`UiExportPage` observes `ExportVideoSystem`'s **system** revision for its
summary; a "Reset presets" button issues `ExportVideoSystem.resetPresets()`
(command), which mutates that system's tables and bumps its revisions —
the page then rebuilds via observation next pass.

---

## 8. Migration from the current 106 pattern

Non-breaking, in slices (mirroring the theme-system migration style):

- **D0 — `Table(T)` + eager revisions.** Give `Table(T)` a
  `tableRevision`, components a `revision`, systems a `revision` +
  `markChanged`. Leave `AppState`/`UiWorld` intact.
- **D1 — `SystemRegistry` + generic `Observe` + one refresh pass.** Add
  the registry, `Observe { system, kind, target, observedRevision }`,
  and a single `refreshObservers(registry)` replacing per-domain
  `refreshXViewsIfNeeded`. Keep the old path in parallel.
- **D2 — carve out systems (plain structs), split eagerly.**
  `MediaLibrarySystem` / `PlaybackSystem` / …; Spotify becomes
  integration functions writing `PlaybackSystem`. Split the UI into
  per-feature UI systems (`UiPlaybackSystem`, `UiMenuBarSystem`, …) and a
  `UiRenderSystem`. Route UI→data writes through command functions.
- **D3 — port consumers.** Convert each widget from
  `DataDependency(enum)` to `Observe(SystemId + target ids)`: play/pause
  icon and **cover → observe `PlaybackSystem.nowPlaying` (fixes the
  motivating bug)**; codec list → observe the `codecs` table. Delete
  `rebindAlbumCoversToSpotify` + the poll's UI writes.
- **D4 — delete the enums.** Remove `DataResourceId`/`DataFieldId`/
  `AppDataResources`/`bumpXResource`.

Each slice keeps the previous mechanism until the next removes it; 106
stays runnable throughout.

---

## 9. Open questions

Most earlier questions are now resolved in the body (split eagerly;
IDs + registry; eager bumping; render system is transient-and-cached,
not per-frame throwaway; explicit not implicit bumping). Remaining:

1. **Command channel shape.** Direct public function call vs a queued
   command/event buffer for UI→data writes. Direct is simplest; a
   command buffer decouples timing (apply all commands at a fixed point
   in the frame) and eases undo/replay. Start with direct calls; add a
   buffer only if a system needs deferred/ordered application.
2. **`SystemRegistry` ownership & borrows.** How the registry holds
   systems and hands out `view`/`mod` access during a refresh without
   aliasing conflicts (one system observing another while a third
   commands it in the same pass). Likely: refresh reads are `view`;
   commands run in a separate phase with `mod`.
3. **Render-cache invalidation.** The precise rule for when a pooled
   `ComputedRect` / glyph layout / GPU buffer may be reused vs must be
   recomputed (observe layout inputs' revisions, not the whole widget).
4. **Future language support (leave open).** Automatic table
   registration, compile-time table reflection, codegen, serialization
   support, or a `system Foo { ... }` sugar generating the boilerplate.
   Useful eventually, but the architecture must work today with plain
   structs + `Table(T)`; do not design around features that don't exist.

---

## 10. Summary of recommendations

1. **`Table(T)` is the only generic primitive.** No runtime `System`
   base. "System" means a **domain module**: a plain struct of
   `Table(T)` fields + a `revision: Int` + free functions.
2. **Split eagerly** into many small systems; integrations that own no
   tables (Spotify) are just functions.
3. **Communicate by one-way observation + commands back**: observe
   another system's revisions (read); to change it, call its public API
   / enqueue a command (the owner mutates its own tables).
4. **Three layers** by lifetime: application-state systems, persistent
   per-feature UI systems, one transient **cache-oriented**
   `UiRenderSystem` (pooled rects, glyph/GPU reuse, virtualization).
5. **Three eagerly-bumped revision levels** — component instance, table,
   system. No per-field, no entity-wide, no implicit/compiler bumping
   (explicit `bumpRevision` or bump-after-change helpers).
6. **References are IDs** (`SystemId` + `SystemRegistry`, table/entity
   ids), never pointers — generic, serializable, hot-reload-friendly.
