# Rae UI — Data Observation & separate ECS worlds — design document

Status: **PROPOSED (design only, no implementation). 2026-07-09.**

The guiding principle this document adopts as the test for every
decision below:

> The UI never owns application data. It **observes** data that lives
> in its own ECS world, and rebuilds only the parts whose observed
> data actually changed.

This supersedes the app-specific resource-enum approach currently in
`examples/106_mobile_ui/data_sources.rae` (`AppDataResources` +
`DataResourceId`/`DataFieldId` + `bumpXResource`). That approach works
but hard-codes one enum per data domain (`albums`, `history`, …,
`spotify`) and wires each refresher by hand — which is exactly why the
Spotify cover bug below exists. The design here generalises it.

---

## 1. Motivating bug (why this matters now)

The 106 mini-player / Now-Playing **cover** does not follow a track
change: play "Chains of Love", then play "Eclipse", and the cover
stays on the first album. Root cause, in the current code:

- Data changes bump a revision (`bumpSpotifyResource` → `spotifyRevision`).
- But **nothing observes `spotifyRevision` to update the cover.** The
  cover is instead repainted by a per-frame poll walk
  (`rebindAlbumCoversToSpotify`) that matches sprites *whose texture key
  still equals the browsed album stem*. The persistent dock is mounted
  once and keeps its original stem, so after a track change the match
  never succeeds and the cover is never re-pointed.
- The title *does* update, because it rides the poll's text writes —
  so the symptom is "title changes, cover doesn't."

The play/pause icon, by contrast, is correct because it *is* on the
revision pattern: `refreshPlaybackViewsIfNeeded` compares an observed
revision against `PlaybackState.revision` and re-applies only on
mismatch. The lesson: **the observation pattern works; the cover just
isn't on it.** Rather than bolt one more bespoke refresher on, this
document proposes the generic mechanism the whole UI should use.

---

## 2. Two ECS worlds

Application data and UI are different concerns with different
lifetimes, ownership, and change rates. Model them as **two ECS
worlds**:

```
UI World (ECS)                 Data World (ECS)
  Button                         ExportSettings
  Text            observes -->   MediaFile
  Image                          Codec
  Window                         Album / Track / Playback / SpotifyNowPlaying
```

- **The UI world never owns data.** A `Text` doesn't store the track
  title; it observes the `Track` (or `SpotifyNowPlaying`) component in
  the Data world and reads the title when it rebuilds.
- **The Data world knows nothing about the UI.** It's just typed
  components in tables, mutated by app logic / integrations
  (filesystem, Spotify, compiler, undo history, AI generation). It has
  no widgets, no layout, no render.
- The two are joined only by **observation links** (§4) that point from
  a UI entity into a Data-world target.

This is a stronger separation than today's `AppState`/`UiWorld` split:
the data becomes *first-class ECS*, so the same query/table/revision
machinery the UI world already uses applies to data too, and
observation becomes uniform instead of a set of hand-written adapters.

Note: "Data world" is a logical ECS world. Whether it is literally a
second `UiWorld`-shaped struct or a distinct `DataWorld` type is an
implementation choice (§8); the design only requires that data lives in
tables with revisions and is addressable by (table, entity).

---

## 3. Three revision levels (and only three)

Every observable change is expressed as one of **three** monotonic
revision counters. This is the core of the proposal.

### 3.1 Component-instance revision — "this object's data changed"

Each component instance carries a `revision`. Any field write bumps it:

```
ExportVideoCodec { bitRate, encoder, profile, level, revision }
  // set any field  ->  revision++
```

- **No per-field versions.** Per-field is overkill: a codec widget that
  shows all four fields would have to OR four counters, and most
  consumers want "did this object change at all?". One counter per
  component instance is cheap and sufficient.
- Granularity is **the component, not the entity** (see 3.4).

### 3.2 Table revision — "objects added/removed/reordered"

Each component *table* carries a `tableRevision`, bumped on structural
change: add entity, remove entity, reorder, sort, filter.

```
Codec table: [entity 4, entity 8, entity 19]
  // add/remove/reorder  ->  tableRevision++
```

This is distinct from instance revisions. A `CodecListView` doesn't
care that one codec's bitrate changed (an instance revision); it cares
that a codec was **added or removed** (the table revision). So it
watches the table, and is untouched by field edits.

### 3.3 System revision — "something in this subsystem changed"

A named *system* groups a set of tables and carries a `systemRevision`
that increments whenever anything inside it changes:

```
ExportSystem { table Codec, table Preset, table Target }
  // any change to any member table or instance  ->  systemRevision++
```

Useful when a consumer doesn't care *what* changed, only that the
subsystem is dirty:

> "Anything export-related changed? Recompute my summary."

A page-level summary observes the system; it recomputes wholesale on
any export change without enumerating which.

### 3.4 Explicitly rejected: entity-wide revision

We do **not** add a per-entity "any component on this entity changed"
revision. It is too coarse:

```
File entity: { Metadata, Thumbnail, Permissions, Waveform }
```

A waveform widget must not redraw because `Permissions` changed. It
observes the `Waveform` **component** on that entity, not the entity.
Component-level is the precise unit; entity-level would reintroduce the
over-invalidation this design removes.

**Summary of the ladder** (coarse → fine):

```
System revision      "something in this subsystem changed"
  ↓
Table revision       "objects added / removed / reordered"
  ↓
Component revision   "this object's data changed"
```

Three levels cover ~99% of real needs without per-field bookkeeping.

---

## 4. Observation: dependencies point at ECS objects, not enums

This is the key improvement over the current design. Today a widget
"depends on `DataResourceId.spotify`" — an arbitrary enum that must be
invented, bumped, and refreshed per app domain. Instead, a dependency
points **directly at an ECS target**:

```
Observe {
  kind:     component | table | system
  target:   (table, entity)  | table | systemId
  observedRevision: Int
}
```

Concrete forms:

```
Observe component ExportVideoCodec(entity 17), rev 34
Observe table     Codec
Observe system    ExportSystem
```

The refresh check is the same three lines regardless of kind:

```
current := revisionOf(observe.target)
if current != observe.observedRevision {
    rebuild the widget from the observed data
    observe.observedRevision = current
}
```

Because dependencies reference ECS objects, the mechanism is **fully
generic**: Spotify now-playing, media library, filesystem, compiler
state, undo history, AI generation — all observe (component / table /
system) targets through the identical path. There is **no**
`SpotifyResource`, `LibraryResource`, `HistoryResource` enum to add per
feature. Adding a new data source is "put components in a table"; the
observation layer needs no new cases.

### 4.1 Relationship to the current `DataDependency`

Today's `DataDependency { resource: DataResourceId, field: DataFieldId,
observedRevision }` is the same shape with an enum target instead of an
ECS target. The migration (§7) is: replace the `(resource, field)`
enum pair with an ECS `(kind, target)`, keep `observedRevision`, and
replace the eight hand-written `bumpXResource` + per-domain refreshers
with one generic revision read + one generic refresh walk.

---

## 5. Worked shapes

### 5.1 A leaf widget (Text / Image)

```
// mount: the now-playing cover image observes the playback component
observe(coverImage, component: SpotifyNowPlaying(playbackEntity))

// refresh pass: rebuild only on mismatch
if rev(SpotifyNowPlaying(playbackEntity)) != coverImage.observed {
    coverImage.textureKey = artKeyOf(SpotifyNowPlaying(playbackEntity))
    coverImage.observed   = rev(...)
}
```

This is exactly the fix the Spotify cover needs: the cover observes the
now-playing *component instance*; when its revision advances (track
changed), the cover node is re-pointed by identity — no stale-stem
matching, no per-frame sprite walk.

### 5.2 A list

```
CodecListView observes table Codec (observedTableRevision).
if tableRev(Codec) != observedTableRevision {
    rebuild children (one row per Codec entity)
    observedTableRevision = tableRev(Codec)
}
```

Field edits inside a codec don't rebuild the list; each row observes
its own `Codec` instance for that.

### 5.3 A page

```
ExportPage observes system ExportSystem for its summary/layout.
Individual controls inside observe their own component instances.
```

Coarse at the top (page recomputes its summary on any export change),
fine at the leaves (a control rebuilds only when its own data changes).

---

## 6. Where sampling still lives (honesty about "no polling")

The **UI-update** side becomes fully event/revision-driven: no
per-frame sprite walks, no per-frame text writes. A refresh pass reads
the three counters and does work only on mismatch.

But some **data sources have no push**: Spotify-over-AppleScript,
polling a filesystem, an external process. For those, the *integration
layer* must still sample on an interval and write the result into the
Data world — at which point a field write bumps the component revision
and the UI reacts. That boundary is inherently pull-based; the design
removes polling from the UI, not from an OS integration that offers no
callback. Where a push/event source exists (a file watcher, a socket,
an in-process mutation), the sampler is replaced by writing the Data
world directly on the event — same downstream path.

So the rule is: **the UI observes; integrations write; revisions are
the only channel between them.**

---

## 7. Migration from the current 106 pattern

Non-breaking, in slices (mirroring the theme-system migration style):

- **D0 — revisions on the Data side.** Give data components an instance
  `revision`, give data tables a `tableRevision`, define a couple of
  systems (`PlaybackSystem`, `LibrarySystem`). Nothing observes yet.
- **D1 — generic `Observe`.** Add the `Observe { kind, target,
  observedRevision }` component and a single generic
  `refreshObservers()` pass that replaces the per-domain
  `refreshXViewsIfNeeded` functions. Keep the old path in parallel.
- **D2 — move the Data world out of `AppState`.** Reshape the app data
  (albums/tracks/playback/spotify/library/history) into Data-world
  tables addressable by (table, entity).
- **D3 — port consumers.** Convert each widget from
  `DataDependency(enum)` to `Observe(ECS target)`: play/pause icon →
  observe playback component; **cover → observe now-playing component
  (fixes the motivating bug)**; history list → observe history table;
  etc. Delete `rebindAlbumCoversToSpotify` and the poll's UI writes.
- **D4 — delete the enums.** Remove `DataResourceId`/`DataFieldId`/
  `AppDataResources`/`bumpXResource` once no consumer references them.

Each slice keeps the previous mechanism until the next removes it, and
106 stays runnable throughout.

---

## 8. Open questions

1. **One world type or two?** Is the Data world a second `UiWorld`
   instance, a distinct `DataWorld` struct, or does `UiWorld` gain
   data tables in a separate namespace? Leaning: a distinct
   lightweight `DataWorld` (tables + revisions, no layout/render), so
   the separation is enforced by the type system.
2. **How is a system's revision maintained?** Derived on read
   (`max` / sum of member table+instance revisions) vs eagerly bumped
   on every member mutation. Eager bump is cheaper to read; derived is
   cheaper to write and can't drift.
3. **Revision width / wraparound.** `Int` monotonic counters are fine
   for a session; define behavior across save/reload (the current code
   already faces this with `playback.revision`).
4. **Cross-world queries.** A refresh reads Data-world components from a
   UI-world pass — needs both worlds in scope. Confirm the ownership /
   borrow story (`view` into the Data world during the UI refresh).
5. **Batching.** Multiple field writes in one frame should bump a
   component revision once, not N times — cheap to get right (bump at
   end of a mutation scope) but worth stating.
6. **Compiler support.** Could Rae make instance-revision bumping
   implicit on `mod` component access, so app code can't forget to
   bump? (Analogous to how `componentMod` already bumps the table
   generation today.) This is the most interesting language-level
   lever.

---

## 9. Summary of recommendations

1. Two ECS worlds: **UI observes, Data is observed**; the UI never owns
   application data.
2. Exactly **three revision levels** — component instance, component
   table, system — and **no** per-field or per-entity-wide revisions.
3. Dependencies point at **ECS objects** (component / table / system),
   not app-specific resource enums, making observation generic across
   every data source.
4. One generic refresh: `if currentRevision != observed { rebuild;
   observed = current }`. No per-frame walks in the UI.
5. Sampling stays only at push-less integration boundaries; it writes
   the Data world and lets revisions propagate.
