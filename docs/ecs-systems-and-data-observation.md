# Rae — ECS systems & cross-system data observation — design document

Status: **PROPOSED (design only, no implementation). 2026-07-09.**

The guiding principle this document adopts as the test for every
decision below:

> An application is a collection of small, cohesive **systems**. A
> system is just a plain struct of `Table(T)` fields plus the functions
> that operate on them. Systems communicate only by **observation**: a
> system may observe entities or tables in another system and rebuild
> the parts whose observed data actually changed. No system owns
> another's data.

This supersedes the app-specific resource-enum approach in
`examples/106_mobile_ui/data_sources.rae`, rejects the earlier
"two worlds" (`UiWorld` + `DataWorld`) draft (a single `DataWorld`
becomes the dumping ground for every table), and also rejects a generic
runtime `System` base type. **The only reusable primitive is
`Table(T)`.**

---

## 1. Motivating bug (why this matters now)

The 106 mini-player / Now-Playing **cover** does not follow a track
change: play one song, then another, and the cover stays on the first
album. Root cause: a data change bumps a revision
(`bumpSpotifyResource`), but **nothing observes it to update the
cover** — the cover is repainted by a per-frame poll walk
(`rebindAlbumCoversToSpotify`) that matches sprites *whose texture key
still equals the browsed album stem*. The persistent dock is mounted
once and keeps its original stem, so after a track change the match
never succeeds. The title updates (it rides the poll's text writes), so
the symptom is "title changes, cover doesn't."

The play/pause icon is correct because it *is* on the revision pattern
(`refreshPlaybackViewsIfNeeded` compares an observed revision against
`PlaybackState.revision`). The pattern works; the cover isn't on it —
and the pattern should be generic, not hand-wired per data domain.

---

## 2. `Table(T)` is the primitive; a system is a plain struct

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

The conceptual hierarchy — **only `Table` is generic**:

```
Application  contains  Systems
System       contains  Tables      (plain struct fields)
Table(T)     contains  Components
```

### 2.1 Why NOT a runtime `System` base

A generic `struct System { tables: ??? }` immediately hits the type
problem: `Table(Codec)`, `Table(Preset)`, `Table(Window)` are all
different types, so a heterogeneous `tables` field needs reflection /
type-erasure / `Any` / compiler-generated tuples / language support —
a huge amount of machinery just to avoid writing three named fields. A
system has almost no *shared state* to hoist into a base anyway. So
systems stay plain structs.

### 2.2 Shared behaviour is free functions, not inheritance

Anything common (like bumping the revision) is an ordinary function:

```rae
func markChanged(sys: mod MediaLibrarySystem) {
  sys.revision = sys.revision + 1
}
```

No `: System`, no vtable. This is deliberately "very Rae" — no
framework forcing an inheritance hierarchy. Plain structs plus the
`Table(T)` primitive are the whole model.

### 2.3 Integrations are functions, not systems

Not every concern is a system that owns tables. **Spotify, for
example, is not a data-owning system** — it's a set of **integration
functions** that control the Spotify app (via AppleScript) and read
state back. Those functions *write into* whichever system owns the
data (the catalog lives in `MediaLibrarySystem`; "what's playing" lives
in a `PlaybackSystem`), and observers react to that system's revisions.
"Is X a system or just functions?" is answered by "does X own tables?":
if not, it's functions.

---

## 3. Three system *layers* (different lifetimes)

Systems fall into three layers, each a plain struct (of tables), each
with a distinct lifetime. Ownership falls out cleanly.

### 3.1 Application-state systems (owned data)

```
MediaLibrarySystem — albums, tracks, artists, thumbnails
PlaybackSystem     — nowPlaying (current track / art / position / isPlaying)
ExportVideoSystem  — codecs, presets, outputFormats, destinations
```

Lifetime: as long as the data is relevant (often the session). Mutated
only by their owning domain logic / integration functions (filesystem,
Spotify, compiler, undo, AI). They know nothing about the UI.

### 3.2 Persistent UI systems (widget state)

There is **not one `UiSystem`.** UI is split per bounded context:

```
UiExportVideoSystem — ExportWindow, BitrateDropdown, CodecList, ProgressBar
UiNowPlayingSystem  — AlbumImage, TrackTitle, PlayButton
```

These hold **persistent widget state**: the widget tree, focus,
selection, expanded tree nodes, scroll position. Lifetime: as long as
the view exists. The data system never knows these widgets exist; the
UI system observes the data system (§5).

### 3.3 The transient render system (per-frame)

UI *state* and UI *rendering* are different data with different
lifetimes and must not be mixed into the persistent widget struct:

```
UiRenderSystem
  visibleRects, clipRects, layoutCache,
  glyphBatches, virtualizedLists, drawCommands,
  textureAtlasRefs  (GPU resources)
```

Lifetime: **transient** — rebuilt every frame / layout pass. This is
where list **virtualization** lives (only visible rows get render
entities), plus clipping, draw commands, glyph batching, GPU handles.
Keeping it separate means the persistent UI system isn't churned every
frame and the render layer can be discarded/rebuilt freely.

### 3.4 The observation chain

```
MediaLibrarySystem / PlaybackSystem   application state
   ↑ observes
UiNowPlayingSystem                    persistent widgets
   ↑ observes
UiRenderSystem                        layout / clip / draw / GPU (transient)
```

Each layer observes the one above and rebuilds only what changed.

---

## 4. Three revision levels (and only three)

Every observable change is one of three monotonic counters, and they
map directly onto the plain-struct model:

### 4.1 Component-instance revision — "this object's data changed"

Each component instance carries a `revision`; any field write bumps it.
**No per-field versions** (one counter per instance is enough).
Granularity is the **component**, not the entity (see 4.4).

### 4.2 Table revision — "objects added / removed / reordered"

`Table(T).tableRevision`, bumped on structural change. A `CodecList`
watches the *table* (adds/removes); a codec row watches its own
*instance*. This lives in the `Table(T)` primitive, so every system
gets it for free.

### 4.3 System revision — "something in this subsystem changed"

Just the plain `revision: Int` field on the system struct, bumped by a
free function (`markChanged`) whenever any member table/instance
changes. Useful for a consumer that only asks "anything export-related
changed? recompute my summary."

### 4.4 Explicitly rejected: entity-wide revision

No per-entity "any component on this entity changed" counter — too
coarse. A `Waveform` widget must not redraw because a `Permissions`
component on the same entity changed; it observes the `Waveform`
component, not the entity.

**Ladder (coarse → fine):** system `revision` → `Table.tableRevision` →
component instance `revision`.

---

## 5. Observation is system-to-system

The generalization that makes this extensible:

> **A system may observe entities or tables in another system.**

```
UiExportVideoSystem  observes  ExportVideoSystem
UiNowPlayingSystem   observes  PlaybackSystem
TimelineSystem       observes  MediaLibrarySystem
UiRenderSystem       observes  UiExportVideoSystem
```

A dependency points **directly at an ECS target in another system**,
never at an app-specific enum:

```
Observe {
  system:  <handle to the observed system>
  kind:    component | table | system
  target:  (table, entity) | table | (whole system)
  observedRevision: Int
}
```

Refresh is the same three lines for every kind and every pair of
systems:

```
current := revisionOf(observe)      // reads the right counter in observe.system
if current != observe.observedRevision {
    rebuild the observing entity from the observed data
    observe.observedRevision = current
}
```

Because targets are `Table(T)`/component/whole-system in named structs,
the mechanism is fully generic: media library, playback, filesystem,
compiler state, undo, AI generation — all observed the same way. There
is **no** `SpotifyResource`/`LibraryResource` enum per feature; adding a
data source is "add a struct with `Table(T)` fields," and the
observation layer needs no new cases.

### 5.1 Relationship to today's `DataDependency`

`DataDependency { resource: DataResourceId, field: DataFieldId,
observedRevision }` is the same shape with an **enum** target. The
migration (§7): replace `(resource, field)` with `(system, kind,
target)`, keep `observedRevision`, and replace the eight `bumpXResource`
+ per-domain refreshers with one generic revision read + one generic
refresh walk.

---

## 6. Worked shapes

### 6.1 Leaf widget (the cover — the motivating fix)

```
// UiNowPlayingSystem: the cover observes PlaybackSystem's nowPlaying instance
observe(cover, system: PlaybackSystem, component: nowPlaying(entity))

// refresh: rebuild by identity on mismatch — no stale-stem matching
if rev(PlaybackSystem, nowPlaying(entity)) != cover.observed {
    cover.artKey = artKeyOf(PlaybackSystem, nowPlaying(entity))
    cover.observed = rev(...)
}
```

The Spotify integration functions write the new track/art into
`PlaybackSystem.nowPlaying` (bumping its instance revision); the cover
observes that instance and re-points by identity. No per-frame sprite
walk, no stale stem.

### 6.2 List (with virtualization spanning two layers)

- `CodecList` (`UiExportVideoSystem`) observes the `codecs` **table** in
  `ExportVideoSystem`; on `tableRevision` change it rebuilds its rows.
- `UiRenderSystem` observes `CodecList`'s scroll state + row set and
  materializes **only the visible** row rectangles (virtualization) into
  transient draw entities. Field edits inside one codec bump that row's
  *instance* revision (row rebuilds), not the list's table revision.

### 6.3 Page

`UiExportPage` observes `ExportVideoSystem`'s **system** revision for its
summary/layout; individual controls observe their own component
instances. Coarse at the top, fine at the leaves.

---

## 7. Where sampling still lives (honesty about "no polling")

The observation side is fully revision-driven: no per-frame walks, work
only on a revision mismatch. But some data sources have **no push** —
Spotify-over-AppleScript, a polled filesystem, an external process. For
those, the owning system's **integration functions** must still
**sample** on an interval and write the result into that system's
tables; the field write bumps the instance/table/system revisions and
observers react. That boundary is inherently pull-based. Where a
push/event source exists (file watcher, socket, in-process mutation),
the sampler is replaced by writing the system on the event — same
downstream path.

Rule: **owning systems (or their integration functions) write their own
tables; other systems observe; revisions are the only channel.**

---

## 8. Migration from the current 106 pattern

Non-breaking, in slices (mirroring the theme-system migration style):

- **D0 — `Table(T)` + revisions.** Give `Table(T)` a `tableRevision`,
  component instances a `revision`, and add a plain `revision: Int` +
  `markChanged` to the systems you carve out. Leave `AppState`/`UiWorld`
  intact.
- **D1 — generic `Observe` + one refresh pass.** Add `Observe { system,
  kind, target, observedRevision }` and a single `refreshObservers()`
  replacing the per-domain `refreshXViewsIfNeeded`. Keep the old path in
  parallel.
- **D2 — carve out systems (plain structs).** Split app data into
  `MediaLibrarySystem` / `PlaybackSystem` / … out of `AppState`; make
  Spotify a set of integration functions writing `PlaybackSystem`. Split
  the UI into per-context UI systems and factor a `UiRenderSystem`.
- **D3 — port consumers.** Convert each widget from
  `DataDependency(enum)` to `Observe(system + ECS target)`: play/pause
  icon → observe `PlaybackSystem.nowPlaying`; **cover → observe
  `PlaybackSystem.nowPlaying` (fixes the motivating bug)**; codec list →
  observe the `codecs` table. Delete `rebindAlbumCoversToSpotify` + the
  poll's UI writes.
- **D4 — delete the enums.** Remove `DataResourceId`/`DataFieldId`/
  `AppDataResources`/`bumpXResource` once nothing references them.

Each slice keeps the previous mechanism until the next removes it; 106
stays runnable throughout.

---

## 9. Open questions

1. **Where does playback data live?** A dedicated `PlaybackSystem`
   ("what's playing right now") vs a `nowPlaying` table inside
   `MediaLibrarySystem`. Leaning `PlaybackSystem`, because playback has a
   different change rate and owner (the Spotify/AppleScript integration)
   than the catalog. Either way the album/track *catalog* is
   `MediaLibrarySystem`, and Spotify itself is integration functions,
   not a system.
2. **`Observe` handle to another system.** How does an observing system
   hold a reference to the observed one without ownership cycles —
   `view` borrow during the refresh, an id/registry, or a handle? This
   is the main plumbing question now that `Table(T)` is settled.
3. **System-revision maintenance.** Derived on read (aggregate of member
   table/instance revisions) vs eagerly bumped by `markChanged`. Eager
   is cheaper to read; derived can't drift.
4. **Render-system reuse.** With `UiRenderSystem` rebuilt per frame, what
   is cached (layout, glyph batches) vs rebuilt, and virtualization must
   keep list render cost O(visible).
5. **Compiler support for revisions.** Could Rae bump an instance
   revision implicitly on `mod` component access (as `componentMod`
   already bumps the table generation today), so app code can't forget?
6. **Later: a `system` sugar (only if metaprogramming arrives).** If Rae
   gains compile-time reflection / structural metaprogramming, a
   declaration like

   ```rae
   system SpotifySystem { playback: Playback, albums: Album }
   ```

   could have the compiler generate the `Table(T)` fields, revisions,
   registration, and serialization. **Design for plain structs today**;
   such sugar would remove boilerplate later *without changing the
   conceptual model*. Do not assume the feature exists now.

---

## 10. Summary of recommendations

1. **`Table(T)` is the only generic primitive** (entities + components +
   `tableRevision` + lookup/iteration). No generic runtime `System`
   base.
2. A **system is a plain struct** of `Table(T)` fields + a `revision:
   Int` + free functions. Integrations that own no tables (e.g. Spotify)
   are just functions, not systems.
3. **Three system layers** by lifetime: application-state systems,
   persistent per-context UI systems, one transient `UiRenderSystem`.
4. **Three revision levels** — component instance, table, system — and
   no per-field or per-entity-wide revisions.
5. Observation is **system-to-system**: dependencies point at ECS
   objects `(system, kind, target)`, never resource enums. One generic
   refresh: `if current != observed { rebuild; observed = current }`.
   Sampling survives only at push-less integration boundaries, inside
   the owning system's functions.
