# Rae — ECS systems & cross-system data observation — design document

Status: **PROPOSED (design only, no implementation). 2026-07-09.**

The guiding principle this document adopts as the test for every
decision below:

> An application is a collection of small, cohesive **ECS systems**
> that communicate only by **observation**. A system may observe
> entities or tables in another system and rebuild the parts whose
> observed data actually changed. No system owns another's data.

This supersedes the app-specific resource-enum approach currently in
`examples/106_mobile_ui/data_sources.rae` (`AppDataResources` +
`DataResourceId`/`DataFieldId` + `bumpXResource`), and it deliberately
rejects the earlier draft of this document that proposed just **two**
ECS worlds (`UiWorld` + `DataWorld`). Two worlds is not extensible: a
single `DataWorld` becomes the dumping ground for every table in the
app.

---

## 1. Motivating bug (why this matters now)

The 106 mini-player / Now-Playing **cover** does not follow a track
change: play "Chains of Love", then play "Eclipse", and the cover
stays on the first album. Root cause, in the current code:

- Data changes bump a revision (`bumpSpotifyResource` → `spotifyRevision`).
- But **nothing observes `spotifyRevision` to update the cover.** The
  cover is repainted by a per-frame poll walk
  (`rebindAlbumCoversToSpotify`) that matches sprites *whose texture key
  still equals the browsed album stem*. The persistent dock is mounted
  once and keeps its original stem, so after a track change the match
  never succeeds and the cover is never re-pointed.
- The title *does* update (it rides the poll's text writes), so the
  symptom is "title changes, cover doesn't."

The play/pause icon is correct because it *is* on the revision pattern
(`refreshPlaybackViewsIfNeeded` compares an observed revision against
`PlaybackState.revision`). The lesson: the observation pattern works;
the cover just isn't on it — and the pattern itself should be made
generic rather than hand-wired per data domain.

---

## 2. Systems, not worlds

The core shift: stop thinking "one UI world + one data world" and think
**many ECS systems**, each a **bounded context**.

```
ExportVideoSystem            SpotifySystem            MediaLibrarySystem
  Codec                        Playback                 MediaFile
  Preset                       Album                    AlbumMetadata
  OutputFormat                 Track                    Folder
  Destination                  Artist                   Thumbnail
```

Each is its **own ECS** — its own entities, component tables,
revisions, and logic — not a shared table bag. Ownership is obvious
(the system that owns a component is the only one that mutates it) and
each subsystem stays cohesive and small. When the app grows you add a
system, not another 500 lines to a monolith.

### 2.1 `System` is a reusable primitive

An ECS container is a value, not a singleton. "World" implies there is
only one; there are many, so the primitive is a `System`:

```
struct ExportVideoSystem {
  System ecs          // composition: the ECS lives in a field
  // ...bounded-context-specific helpers/config
}
```

(Whether Rae expresses this as composition — a `System` field, the
leaning here — or as some `ExportVideoSystem : System` form depends on
what the language offers; see §8.) The point is that `System` provides,
uniformly for every subsystem:

- entities,
- component tables (each with a **table revision**),
- component instances (each with an **instance revision**),
- a single aggregate **system revision**,
- and its own update logic.

`UiWorld` today is one such ECS; under this design it becomes several
`System`s (§3), each built from the same primitive.

---

## 3. Three system *layers* (different lifetimes)

Not all systems are the same kind. There are three layers, each a
separate ECS with a distinct lifetime. Ownership falls out cleanly.

### 3.1 Application-state systems (owned data)

```
SpotifySystem        — Playback, Album, Track, Artist
ExportVideoSystem    — Codec, Preset, OutputFormat, Destination
MediaLibrarySystem   — MediaFile, AlbumMetadata, Folder, Thumbnail
```

Lifetime: as long as the data is relevant (often the whole session).
Mutated only by their owning domain logic / integrations (filesystem,
Spotify, compiler, undo, AI). They know nothing about the UI.

### 3.2 Persistent UI systems (widget state)

There is **not one `UiSystem`.** UI is split per bounded context, one UI
system per application-state system it presents:

```
UiExportVideoSystem  — ExportWindow, BitrateDropdown, CodecList, ProgressBar
UiSpotifySystem      — AlbumImage, TrackTitle, PlayButton
```

These hold **persistent widget state**: the widget tree, focus,
selection, expanded tree nodes, scroll position. Lifetime: as long as
the view exists (survives across frames and data changes). The data
system never knows these widgets exist; the UI system observes the data
system (§5).

### 3.3 The transient render system (per-frame)

This is the layer the earlier draft missed. UI *state* and UI
*rendering* are different data with different lifetimes, and must not be
mixed into the persistent widget ECS:

```
UiRenderSystem
  VisibleRect        ClipRect          LayoutCache
  GlyphBatch         VirtualizedList    DrawCommand
  TextureAtlasReference   (GPU resources)
```

Lifetime: **transient** — recreated every frame (or every layout pass).
This is where list **virtualization** lives (only the visible
rectangles of a long list get render entities), where clipping, draw
commands, glyph batching, and GPU handles live. Keeping it a separate
ECS means the persistent `UiExportVideoSystem` isn't churned every
frame, and the render layer can be rebuilt/discarded freely.

So a control's identity + scroll position live in the persistent UI
system; the *rectangles actually drawn this frame* (including the
virtualized subset of a big list) live in the render system.

### 3.4 The three layers, and their observation chain

```
ExportVideoSystem        application state          (owned by domain)
   ↑ observes
UiExportVideoSystem       persistent widgets         (owned by UI)
   ↑ observes
UiRenderSystem            layout / clip / draw / GPU (transient)
```

Each layer observes the one above and rebuilds only what changed: the
render system rebuilds draw data when widget state changes; the widget
system rebuilds widgets when application data changes.

---

## 4. Three revision levels (and only three)

Every observable change is one of **three** monotonic counters, and
they compose with the layering: every `System` exposes all three.

### 4.1 Component-instance revision — "this object's data changed"

Each component instance carries a `revision`; any field write bumps it.

```
ExportVideoCodec { bitRate, encoder, profile, level, revision }
  // set any field  ->  revision++
```

- **No per-field versions** (overkill: one counter per instance is
  enough; consumers almost always want "did this object change?").
- Granularity is the **component**, not the entity (see 4.4).

### 4.2 Table revision — "objects added / removed / reordered"

Each component table carries a `tableRevision`, bumped on structural
change (add / remove / reorder / sort / filter).

```
Codec table: [entity 4, entity 8, entity 19]
  // add/remove/reorder  ->  tableRevision++
```

A `CodecList` watches the *table* (adds/removes), not each codec's
fields; a codec row watches its own *instance*.

### 4.3 System revision — "something in this subsystem changed"

Each `System` carries one aggregate `systemRevision`, bumped whenever
anything inside it (any member table or instance) changes. This is the
natural third level once systems are the unit: it *is* the bounded
context's revision.

```
ExportVideoSystem.systemRevision   // ticks on any Codec/Preset/... change
```

Useful when a consumer doesn't care what changed, only that the
subsystem is dirty: "anything export-related changed? recompute my
summary."

### 4.4 Explicitly rejected: entity-wide revision

We do **not** add a per-entity "any component on this entity changed"
counter. It's too coarse:

```
MediaFile entity: { Metadata, Thumbnail, Permissions, Waveform }
```

A waveform widget must not redraw because `Permissions` changed — it
observes the `Waveform` **component**, not the entity. Component-level
is the precise unit.

**Ladder (coarse → fine):** system → table → component instance.

---

## 5. Observation is system-to-system

The generalization that makes this extensible: observation is **not**
"UI observes Data." It is:

> **A system may observe entities or tables in another system.**

```
UiExportVideoSystem   observes   ExportVideoSystem
UiSpotifySystem       observes   SpotifySystem
TimelineSystem        observes   MediaLibrarySystem
WaveformSystem        observes   AudioAnalysisSystem
UiRenderSystem        observes   UiExportVideoSystem
```

Nothing in the observation mechanism knows what kind of system either
side is. Every system merely exposes entities, component tables, and
the three revisions. A dependency points **directly at an ECS target in
another system**, never at an app-specific enum:

```
Observe {
  system:  <handle to the observed System>
  kind:    component | table | system
  target:  (table, entity) | table | (whole system)
  observedRevision: Int
}
```

Refresh is the same three lines for every kind and every pair of
systems:

```
current := revisionOf(observe)          // reads the right counter in observe.system
if current != observe.observedRevision {
    rebuild the observing entity from the observed data
    observe.observedRevision = current
}
```

Because targets are ECS objects in named systems, the mechanism is
fully generic: Spotify, media library, filesystem, compiler state, undo
history, AI generation — all observed the same way. There is **no**
`SpotifyResource` / `LibraryResource` enum to invent per feature;
adding a data source is "spin up a `System` with tables," and the
observation layer needs no new cases.

### 5.1 Relationship to today's `DataDependency`

`DataDependency { resource: DataResourceId, field: DataFieldId,
observedRevision }` is the same shape with an **enum** target. The
migration (§7): replace `(resource, field)` with `(system, kind,
target)`, keep `observedRevision`, and replace the eight
`bumpXResource` + per-domain refreshers with one generic revision read
+ one generic refresh walk.

---

## 6. Worked shapes

### 6.1 Leaf widget (the cover — the motivating fix)

```
// UiSpotifySystem: the cover widget observes the Spotify Playback instance
observe(cover, system: SpotifySystem, component: Playback(playbackEntity))

// refresh: rebuild by identity on mismatch — no stale-stem matching
if rev(SpotifySystem, Playback(playbackEntity)) != cover.observed {
    cover.artKey = artKeyOf(SpotifySystem, Playback(playbackEntity))
    cover.observed = rev(...)
}
```

This is exactly the cover fix: the cover observes the Playback
*instance*; on a track change its instance revision advances and the
cover node is re-pointed by identity. No per-frame sprite walk.

### 6.2 List (with virtualization spanning two layers)

- `CodecList` (in `UiExportVideoSystem`) observes the `Codec` **table**
  in `ExportVideoSystem`; on `tableRevision` change it rebuilds its row
  widgets.
- `UiRenderSystem` observes `CodecList`'s scroll state + the row set and
  materializes **only the visible** row rectangles (virtualization) into
  transient `DrawCommand`/`VisibleRect` entities.

Field edits inside one codec bump that row's *instance* revision (the
row rebuilds); they do not touch the list's table revision.

### 6.3 Page

`UiExportPage` observes `ExportVideoSystem`'s **system** revision for
its summary/layout; individual controls observe their own component
instances. Coarse at the top, fine at the leaves.

---

## 7. Where sampling still lives (honesty about "no polling")

The observation side is fully revision-driven: no per-frame walks, work
happens only on a revision mismatch. But some data sources have **no
push** — Spotify-over-AppleScript, a polled filesystem, an external
process. For those the owning application-state system's integration
layer must still **sample** on an interval and write the result into its
own tables; that field write bumps the instance/table/system revisions
and observers react. That boundary is inherently pull-based. Where a
push/event source exists (file watcher, socket, in-process mutation),
the sampler is replaced by writing the system directly on the event —
same downstream path.

Rule: **owning systems write their own tables; other systems observe;
revisions are the only channel between systems.**

---

## 8. Migration from the current 106 pattern

Non-breaking, in slices (mirroring the theme-system migration style):

- **D0 — revisions everywhere.** Give component instances a `revision`,
  tables a `tableRevision`, and each `System` a `systemRevision`. Add
  the `System` primitive; leave the current `AppState`/`UiWorld` intact.
- **D1 — generic `Observe` + one refresh pass.** Add `Observe { system,
  kind, target, observedRevision }` and a single `refreshObservers()`
  that replaces the per-domain `refreshXViewsIfNeeded` functions. Keep
  the old path in parallel.
- **D2 — carve out systems.** Split app data into bounded-context
  systems (`SpotifySystem`, `MediaLibrarySystem`, …) out of `AppState`.
  Split the UI into per-context UI systems and factor a `UiRenderSystem`
  for transient layout/clip/draw/virtualization.
- **D3 — port consumers.** Convert each widget from
  `DataDependency(enum)` to `Observe(system + ECS target)`: play/pause
  icon → observe `SpotifySystem.Playback`; **cover → observe
  `SpotifySystem.Playback` (fixes the motivating bug)**; codec list →
  observe the `Codec` table; etc. Delete `rebindAlbumCoversToSpotify`
  and the poll's UI writes.
- **D4 — delete the enums.** Remove `DataResourceId`/`DataFieldId`/
  `AppDataResources`/`bumpXResource` once nothing references them.

Each slice keeps the previous mechanism until the next removes it; 106
stays runnable throughout.

---

## 9. Open questions

1. **`System` as base or member?** Composition (`struct
   ExportVideoSystem { System ecs }`) vs an inheritance/trait form
   (`ExportVideoSystem : System`). Depends on what Rae offers; leaning
   composition. This is the central language-shape question.
2. **How many UI systems, and how shared?** One `Ui<Context>System` per
   application-state system, plus one shared `UiRenderSystem`? Or a
   small fixed set? Where do cross-context chrome (nav bar, dialogs)
   live?
3. **System-revision maintenance.** Derived on read (aggregate of member
   table/instance revisions) vs eagerly bumped on every member
   mutation. Eager is cheaper to read; derived can't drift.
4. **Cross-system observation ergonomics.** A refresh in one system
   reads components in another — confirm the ownership/borrow story
   (`view` into the observed system during the refresh) and how a
   system handle is held without creating cycles.
5. **Render-system rebuild cost.** If `UiRenderSystem` is recreated per
   frame, what's reused via caches (`LayoutCache`, glyph batches) vs
   rebuilt? Virtualization must make list render cost O(visible), not
   O(total).
6. **Compiler support.** Could Rae bump an instance revision implicitly
   on `mod` component access (as `componentMod` already bumps the table
   generation today), so app code can't forget? Most interesting
   language-level lever.
7. **Revision width / reload.** Monotonic `Int` per session; define
   behaviour across save/reload (the current `playback.revision` already
   faces this).

---

## 10. Summary of recommendations

1. Not two worlds — **many bounded-context ECS systems** built from a
   reusable `System` primitive; each owns its tables, no monolithic
   `DataWorld`.
2. **Three system layers** by lifetime: application-state systems,
   persistent per-context UI systems, and one transient `UiRenderSystem`
   (layout / clip / draw / virtualization / GPU).
3. **Three revision levels** — component instance, table, system — and
   no per-field or per-entity-wide revisions.
4. Observation is **system-to-system**: dependencies point at ECS
   objects `(system, kind, target)` in another system, never at
   resource enums, so the mechanism is generic across every data source
   and UI layer.
5. One generic refresh: `if current != observed { rebuild; observed =
   current }`. Sampling survives only at push-less integration
   boundaries, inside the owning system.
