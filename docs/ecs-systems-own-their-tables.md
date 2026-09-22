# Systems own their tables: splitting UiWorld, app components, and bundles

Status: **design, approved direction** (2026-09-21; supersedes the
"extension world" draft of the same day). **Step 1 (§3.2, the two outboxes)
is implemented** (2026-09-22): `world.pendingComponents` /
`applyComponentInto` / `reportUnknownComponent` / `reportPendingComponents`
in `lib/ui/Registry.rae`, `world.deadEntities` / `releaseDeadEntities` in
`lib/ui/Ecs.rae` (the allocator's `freeEntity` split into `retireEntity` +
`recycleEntityIndex`, `lib/ecs/World.rae`), `uiFrameEnd` for apps with no
tables of their own; the editor keeps what it does not know (inspector line,
info rows, `N app components kept` in its mount/reload log). One deviation
from the text below: `applyComponentInto` also takes `world: mod UiWorld`,
because decoding needs the theme (token values) and the diagnostics list.
**Step 2 (the editor's systems own their tables) is implemented** (2026-09-22):
`ChromeSystem` (5 tables + page root + the bound-label fields), `GuideSystem`
(guideRects), `InspectorSystem` (outlineFors + hover/selection), `OutlinePoolSystem`
(outlinePools), plus `CameraSystem` / `ViewportSystem` / `TextureSystem` renamed
from their ad-hoc state structs; `EditorSystems.rae` holds the hand-written
`applyEditorComponents` / `sweepEditorEntities`; the eight components live in
`examples/121_ui_editor/chromeSystem/EditorComponents.rae`, off `UiWorld`
(83 known components now, 81 registered). Update functions that only read a
system take it `view` (15 of the 29 chrome-system parameters).
**Step 3 (rounded covers as data, §4) is implemented** (2026-09-22): the eight
cover nodes author `CornerRadius` + `AlbumCover {}`; `examples/106_mobile_ui/
albumCoverSystem/AlbumCoverSystem.rae` owns the marker and the authored radii it
squared; `MusicSystems.rae` is 106's hand-written glue (`applyMusicComponents`,
`sweepMusicEntities`, `musicFrameEnd`); `lib/ui/coverStyleSystem/` and
`CoverStyle` are gone, and so is the editor's call.
**Step 4 (106's systems own their tables) is implemented** (2026-09-22):
`PlaybackSystem` (absorbs `PlaybackState`), `DockSystem`, `AlbumSystem`,
`ProfileSystem`, `HistorySystem`, `HomeSystem` + `AlbumCoverSystem`, aggregated
in `MusicSystems` on the 106 `App`; `SpotifyState` → `SpotifySystem`; the eleven
components moved next to their systems and off `UiWorld` (71 known components);
the observers add their own table's generation (`ownTableGeneration`). Decided
while doing it: `PlayHistory` / `HistoryArtLoader` stay on `AppState` (app data,
not the binding's); the app's systems are ONE `MusicSystems` struct so the
observation boundary takes one parameter, not seven.
**Step 5 (bundles, §5) is implemented** (2026-09-22; `docs/ui-scene-format.md`
§9): `lib/ui/SceneBundles.rae` (BundleSet, BundleRefs, the JSON-level
field-wise expansion, `sceneImportedBundles`) + `lib/ui/BundleApply.rae`
(`applyExpandedBag`, `applyBundle`), both loaders expand a node that lists
`bundles`, `world.bundles` is seeded by the hosts like the theme; the editor's
inspector shows the provenance line. NOT yet: the save-side per-field diff —
the editor has no save path (queued as its own task; `BundleRefs` carries what
it needs). `ContainerStyle` is untouched.
**Step 6a (LayoutSystem, SafeAreaSystem, HierarchySystem own their derived
tables) is implemented** (2026-09-22): `lib/ui/UiSystems.rae` holds the three
system types and the `UiSystems` aggregate (`createUiSystems`,
`clearUiSystemsEntity`); `measuredSizes` / `measuredTexts` / `computedRects` /
`layoutScales` / `safeInsets` / `parents` are gone from `UiWorld` (`childrens`
stays); every reader takes `uiSystems: view UiSystems`, every writer `mod`, the
apps own it next to their world (121 `App.uiSystems`, 106 `App.uiSystems`
rebuilt with each route's world, 114 `InputSystem.uiSystems`, the UiShell
examples a local). The schedule keeps its judgement: `uiShouldRun(schedule,
world, uiSystems, index)` adds `uiSystemsReadGeneration` — the system tables
each stage reads, listed by hand in Pipeline.rae — to the declared world
generation (`shouldRunDeclaredPlus`); fixture 839 proves the declared schedule
still equals the hand-written caches on every frame (its declared counts drop
by the moved tables: layout 9 reads / 0 writes, transform 5 / 3).
`releaseDeadEntities(world, uiSystems)` clears the systems' rows with the
entity. Not the pipeline's *signatures* per system (`layoutSystem(world,
uiSystems)` rather than `layoutSystem(world, layoutSystem)`): one aggregate
parameter everywhere was the simple form; splitting it per system is a
follow-up if it earns its churn.
**Step 6b is implemented** (2026-09-22): `Transform2dSystem { localTransforms,
worldTransforms, worldVisuals, runtimeOffsets }`, `AnimationSystem { animStates,
smokeStates, carouselStates }`, `HeroTransitionSystem { heroTransitions }`,
`ButtonSystem { interactions }` on `UiSystems`; the button behaviour is
gathered in `lib/ui/buttonSystem/` — `buttonInteractionClear/Mark` (was
inputSystem), `buttonStyleUpdate` (ButtonStyle.rae, was widgetStyleSystem),
`buttonHoverScaleUpdate` (ButtonHoverScale.rae, was hoverScaleSystem) — as ONE
type and module, not one function: the three keep their pipeline stages
(style before layout, hover scale after transform) because merging them
would move the hover scale a frame earlier and change what is drawn. Stage
order and 839's judgement unchanged (transform's declared counts drop to its
own tables). `UiWorld` now holds only authored tables, `childrens`, the
resources and the allocator.

Prompted by the UI editor having to
call a music-player system to draw a scene the way the music player does.
Related: `ecs-general-architecture.md`, `compile-time-reflection.md`,
`ui-editor-design.md`, `ui-scene-format.md`. The C++ ancestor of this shape
is the maintainer's `rae_ui` project (`ISystem` owns `Table<T>`s and child
systems; `UIScene::createButton` is a code-side bundle) — this design is that
structure in Rae terms, with the callbacks replaced by explicit data.

## 1. The three words

- **Component table** — storage: a sparse set of one component type keyed by
  `EntityId` (`lib/ecs/ComponentTable.rae`). It does not care which struct it
  is a field of; the ids come from the world's allocator either way.
- **System** — a unit of behaviour *together with the state it owns*: its
  update function(s) plus the resources and component tables nothing else
  writes. `type PlaybackSystem { likedHearts: ComponentTable(LikedHeart) … }`
  and `func playbackUpdate(playbackSystem: mod PlaybackSystem, world: mod
  UiWorld, …)`. Classic ECS says a system is only the function; every real
  engine hangs state off it, and Rae's no-globals rule makes that state need
  an owner anyway. Naming the owner after the system is the honest spelling:
  the folder `playbackSystem/`, the type `PlaybackSystem`, the parameter
  `playbackSystem` — three spellings of one thing (the AGENTS.md naming rule).
  A system with no state of its own is just a function; no empty struct.
- **World** — the entity allocator, the resources, and the tables of the
  *shared, authored* components many systems read (`Rect`, `Size`, `Text`,
  `Sprite`, `Shape`, `Children`, …). It exists because those have many readers
  and no single owner. `UiWorld` keeps exactly that role and nothing more:
  **not "all tables", but "the tables with no better owner".**

The fact that decides it: every app-specific table in 106 and 121 is read by
exactly one module (the survey below), so every one of them has an obvious
owner. Most of the lib's *authored* tables do not. Its *derived* tables do.

## 2. Where things are today

`lib/ui/BindingComponents.rae` declares 29 authorable components, of which
nine are generic. Each of the other twenty is also a `ComponentTable` on
`UiWorld` (`lib/ui/Ecs.rae`) and a line in `lib/ui/RegistryFlags.rae`, plus
one app *system* in lib (`lib/ui/coverStyleSystem/`).

| owner today | table on UiWorld | the one module that reads it |
|---|---|---|
| 106 | playbackIcons, playbackCovers, likedHearts, trackTexts, playbackTimes | `playbackSystem/PlaybackBindings.rae` |
| 106 | navTabs | `screenSystem/DockSystems.rae` |
| 106 | albumHeaders | `AlbumSystems.rae` |
| 106 | profileStats, avatarSources | `ProfileView.rae` |
| 106 | historyLists | `HistoryView.rae` |
| 106 | hasHistories | `HomeView.rae` |
| 106 | coverStyles | `lib/ui/coverStyleSystem/` (!) |
| 121 | editorPanels, panelToggles, editAreas, editorLists, diagnosticsBadges | `chromeSystem/ChromeSystem.rae` |
| 121 | guideRects | `guideSystem/GuideSystem.rae` |
| 121 | outlineFors | `inspectorSystem/InspectorSystem.rae` |
| 121 | outlinePools | `outlineSystem/OutlinePoolSystem.rae` |
| lib | heroTransitions | `HeroTransitionSystem.rae` (writer) + `FrameWaitSystem.rae` (reader) |

Why it ended up so: "the registry IS the world" (`lib/ui/Registry.rae`) —
the `.raescene` loader is a compile-time loop over `fields(world)` for the
closed `UiWorld`, so a component was authorable iff it was a lib table.

And the per-system state already exists under ad-hoc names: 121 has
`EditorCamera`, `GuideState`, `InspectorState`, `TextureSearch`,
`EditorStatus`; 106 has `PlaybackState`, `DebugSystem`, `InputSystem`,
`MediaLibrarySystem`. `App` in the editor is literally a struct of these.
This design makes that uniform and lets the system's own tables live there.

**The rule:** a component or system goes in `lib/ui` only if a UI in general
needs it. Domain things (album, track, playback, avatar, the editor's panels)
live with the app that has the domain, on the system that reads them.
Corollary: **the editor runs the lib pipeline and nothing else**; what an app
wants a scene to look like in the editor is authored in generic components.

## 3. The shape

```rae
# examples/106_mobile_ui/playbackSystem/PlaybackSystem.rae
type PlaybackSystem {
  # its authored components (were UiWorld tables)
  playbackIcons: ComponentTable(PlaybackIcon)
  playbackCovers: ComponentTable(PlaybackCover)
  likedHearts: ComponentTable(LikedHeart)
  trackTexts: ComponentTable(TrackText)
  playbackTimes: ComponentTable(PlaybackTime)
  # its state (was the ad-hoc resource PlaybackState)
  position: Float
  playing: Bool
  ...
}

func likedHeartUpdate(
  playbackSystem: mod PlaybackSystem   # its own tables + state
  world: mod UiWorld                   # the shared authored tables it writes (sprites, texts)
  liked: view Playlist
) ret Bool
```

- `mod` on what the function owns and writes, `view` on what it only reads.
  A table read by a second system is owned by one and *viewed* by the other:
  `frameWaitUpdate(heroTransitionSystem: view HeroTransitionSystem, …)`.
  That is the `mod`/`view` rule doing exactly what it is for, one level down.
- The App is the aggregate: `type MusicApp { world: UiWorld, playbackSystem:
  PlaybackSystem, dockSystem: DockSystem, … }`. There is no `MusicWorld`.
  Threading is explicit, as every resource already is (no globals).
- **106 gets 7 systems with tables, 121 gets 4** (the table in §2). The
  systems without tables (`frameSystem/`, `listSystem/`, `projectSystem/`)
  stay functions.

### 3.1 The lib splits the same way

`UiWorld` keeps the authored shared tables, the hierarchy (`childrens`, the
authoritative one), the resources and the allocator. Its *derived* tables
move to the system that derives them:

| lib system (type) | derived tables it owns today on UiWorld |
|---|---|
| `LayoutSystem` | measuredSizes, measuredTexts, computedRects, layoutScales |
| `Transform2dSystem` | localTransforms, worldTransforms, worldVisuals, runtimeOffsets |
| `SafeAreaSystem` | safeInsets |
| `HierarchySystem` | parents (derived from `Children`) |
| `AnimationSystem` | animStates, smokeStates, carouselStates |
| `ButtonSystem` | interactions (hovered/pressed) — plus the press/hover behaviour now spread over inputSystem, hoverScaleSystem and widgetStyleSystem (HoverScale application, StateStyle swap) |
| `HeroTransitionSystem` | heroTransitions (stays generic: a shared-element transition keyed by `HeroWidget`, driving a plain Sprite + CornerRadius) |

`Transform2dSystem` is spelled with the dimension because a
`Transform3dSystem` is the obvious sibling when the 3D side adopts this
shape. The lib's aggregate is `type UiSystems { layout: LayoutSystem,
transform2d: Transform2dSystem, … }`, owned by the app (`app.uiSystems`) or
by `UiShell`, and `runUiPipeline(world: mod UiWorld, uiSystems: mod
UiSystems, …)`. `Pipeline.rae`'s declared read/write sets become what the
signatures already say.

### 3.2 No callbacks: outboxes

Rae has no function references, and this design needs none. Two places in
lib used to reach "everything" through the closed world; both become data
the app drains.

**Loading — the pending-components outbox.** `applyComponentByName` applies
the keys that name a `UiWorld` table, as today, and instead of erroring on
the rest it appends `PendingComponent { entity, name, json }` (json = the
object's source text) to `world.pendingComponents`. After `loadSceneFile`
returns, the app applies its own:

```rae
# examples/106_mobile_ui/MusicComponents.rae — the app's registry, by hand.
func applyMusicComponents(app: mod MusicApp) {
  loop let pending: view PendingComponent in app.world.pendingComponents {
    if applyComponentInto(PlaybackSystem, system: app.playbackSystem, pending: pending) { continue }
    if applyComponentInto(DockSystem, system: app.dockSystem, pending: pending) { continue }
    ...  # one line per system with tables (7 in 106, 4 in 121)
    reportUnknownComponent(world: app.world, pending: pending)   # the #941 error, now the app's
  }
  app.world.pendingComponents.clear()
}
```

`applyComponentInto(S: type, system: mod S, pending: view PendingComponent)
ret Bool` is ONE generic lib function — the existing compile-time loop over
`fields(system)` for `ComponentTable(any)`, decoding structurally as
`RegistryGeneric.decodeComponent` does now — instantiated per system type.
**The editor never drains the outbox**: what stays in it IS the opaque
"components the editor does not know" — preserved for save round-trip,
listed read-only in the inspector, counted as an info diagnostic. One
mechanism, both uses.

**Destroying — the dead-entities outbox.** `destroyEntity` /
`destroyEntitySubtree` / `unmountPage` clear the `UiWorld` tables and push
the id to `world.deadEntities`. The allocator does **not** recycle an id
until the app has confirmed the sweep:

```rae
func sweepMusicEntities(app: mod MusicApp) {
  loop let entity: view EntityId in app.world.deadEntities {
    clearEntityComponents(PlaybackSystem, world: app.playbackSystem, entity: entity)   # exists today (#760)
    ...  # one line per system with tables
  }
  releaseDeadEntities(world: app.world)   # ids become reusable
}
```

Called once per frame after the app's systems (and after any unmount). This
is the `rae_ui` `ISystem::destroyEntities(const Array<Id>&)` pattern, with
the array made explicit. An app with no tables of its own calls
`releaseDeadEntities` alone (UiShell does it for the 3D examples).

**Snapshot** (`worldToJson`/`worldFromJson`, #768): the same shape — lib
serialises `UiWorld`, `serializeInto(S: type, …)` per system, listed by hand.

The hand-written lists are three functions of ≤ 7 lines per app. Once the
shape is proven, `fields()` can be extended to recurse one level into
struct-typed fields, and a single `fields(app.systems)` walk replaces the
lists; nothing in a system signature changes when that happens. **Decided:
hand-written first, `fields()` extension later.**

## 4. The worked example: rounded album covers

Today the cover node authors `CoverStyle { radius: 52 }` and *no*
`CornerRadius`; `coverStyleSystem` (lib) adds the radius when the app's
`roundedAlbumCovers` setting is on. The scene as authored is square, and the
editor rounds it only because it calls the 106 system on mount — the wrong
direction, removed by this design.

```jsonc
"Hero": {
  "Rect": { ... },
  "Sprite": { "textureKey": "albumCover", "scaleMode": "Fill" },
  "CornerRadius": { "radius": 52.048193 },   // generic: every renderer rounds it
  "AlbumCover": {}                            // 106 marker on AlbumCoverSystem
}
```

`AlbumCoverSystem { albumCovers: ComponentTable(AlbumCover),
authoredRadii: ComponentTable(AuthoredRadius) }` in 106: when the setting
turns rounding **off** it records each `AlbumCover` node's `CornerRadius` in
`authoredRadii` and removes it; **on** restores it. Change-disciplined as
today (writes only on a transition; the #981 pixel gate keeps proving it).
Every "the app decides the look at runtime" component follows this pattern:
the scene carries the default in generic components, the app's system varies
it and remembers what it overrode.

## 5. Bundles: named component sets in `.raescene`

The editor's `LeftToggle` pill is ten keys — `Rect, Size, Shape, Layout,
Padding, HitArea, OnClick, Children, PanelToggle, Shadow` — and every other
pill repeats seven of them. The `rae_ui` answer was `createButton(text,
handler)`: a code-side bundle. Rae has two partial answers already:
`ContainerStyle` (a theme-level bundle expanding to Shape + Padding, #237)
and `SceneInstance` (a whole sub-scene as a prefab, with per-field
overrides). What is missing is the general one: a **named set of components
with values, applied to a node, in data.**

Name: **bundle**, not archetype. In ECS an *archetype* is the storage term
for "the set of component types an entity has" (Bevy/DOTS chunk layout);
Rae's tables are sparse sets, so the word would promise a storage model that
does not exist. A *bundle* is Bevy's word for exactly this: components
inserted together with values.

```jsonc
// Bundles.raescene (or any scene's top level; shared through `import`, like Theme)
"bundles": {
  "pillButton": {
    "Size":    { "h": 36 },
    "Shape":   { "fillSlot": "surfaceRaised", "radius": "radiusPill" },
    "Padding": "insetPill",
    "HitArea": {},
    "Shadow":  { "blur": 8, "y": 2 }
  },
  "editorPanelToggle": {
    "bundles": ["pillButton"],                 // a bundle may include bundles
    "PanelToggle": {}
  }
}

// a node applies one or more, then its own components override field-wise
"LeftToggle": {
  "bundles": ["editorPanelToggle"],
  "Rect": { "x": 12, "y": 8, "w": 96, "h": 36 },
  "OnClick": { "actionId": "toggleLeft" },
  "PanelToggle": { "panel": "left" }             // field-wise over the bundle's {}
}
```

Rules:

- **Expansion at load, field-wise.** A bundle is expanded when the node is
  deserialised: for each component the bundle names, decode the bundle's
  object, then apply the node's own object for that component **over** it
  field by field (node wins). Bundles listed later win over earlier ones.
  This is the ContainerStyle expansion generalised to any component and any
  values, using the same `decodeComponent` path.
- **Systems never see bundles.** After expansion the world holds ordinary
  components; no system, and no pipeline stage, knows the word. This keeps
  the "few special cases" promise: a bundle is a scene-format feature only.
- **The name is kept for round-trip.** The node gets `BundleRefs { names }`
  (a lib component, authored-derived: written by the loader, never by
  hand). The editor's save emits `"bundles": [...]` plus only the fields
  that differ from the expansion; the inspector shows bundle-provided values
  as inherited (dimmed) and node-authored ones as overrides — the prefab
  override model the reference editor uses, at component granularity.
- **App components in bundles.** A bundle may name a component the lib does
  not know (`PanelToggle`); the expansion routes it to the pending outbox
  like any unknown key, with the node's field-wise override already merged.
- **Code-side application** is the same expansion on a live world:
  `applyBundle(world, entity, name)` — so `DebugGpu2dMenu` and any code-built
  UI can use the authored bundles instead of listing components by hand.
- **`ContainerStyle` is a bundle** whose values are theme tokens; it stays
  as-is (working, tested) and becomes an entry in the theme's `bundles`
  block in a later cleanup, not now.

## 6. Migration — one queue item per line, every step leaves the tree green

1. **Outboxes in lib.** `pendingComponents` + `applyComponentInto(S: type,
   …)` + `reportUnknownComponent`; `deadEntities` + `releaseDeadEntities`
   with deferred id reuse; the existing apps call the release each frame;
   the editor lists what stays pending (opaque components, inspector rows,
   info diagnostic, save round-trip). No table moves yet.
2. **The editor's four systems.** `ChromeSystem`, `GuideSystem`,
   `InspectorSystem`, `OutlinePoolSystem` types own their eight tables; the
   existing ad-hoc state (`EditorStatus`, `GuideState`, `InspectorState`,
   `EditorCamera` → `CameraSystem`, …) folds into them; `applyEditorComponents`
   / `sweepEditorEntities` by hand; the eight tables leave `UiWorld`,
   `RegistryFlags` and `BindingComponents`. The editor is the smallest real
   user, so it goes first.
3. **Album covers as data** (§4): `CornerRadius` authored in the 106 cover
   scenes, `AlbumCoverSystem` in 106, `coverStyleSystem` deleted from lib,
   the editor's call deleted.
4. **106's seven systems** (`PlaybackSystem`, `DockSystem`, `AlbumSystem`,
   `ProfileSystem`, `HistorySystem`, `HomeSystem`, `AlbumCoverSystem`)
   own their tables and absorb their state structs; the twelve tables leave
   lib; `HeroTransition` stays.
5. **Bundles** (§5): the `bundles` block, node `bundles`, expansion,
   `BundleRefs`, editor round-trip + inspector, `applyBundle`; the editor's
   pills and 106's buttons converted as the proof.
6. **Lib derived tables → lib systems** (§3.1), in two halves: (a)
   `LayoutSystem` + `SafeAreaSystem` + `HierarchySystem` + `UiSystems` +
   the pipeline signature; (b) `Transform2dSystem` + `AnimationSystem` +
   `ButtonSystem`.
7. **Docs sweep**: `ecs-general-architecture.md`, `ecs-api-reference.md`,
   `ui-ecs-106-reference.md`, `ui-editor-design.md`, `ui-scene-format.md`,
   the AGENTS.md "registry IS the world" note.

Order: 1 → 2 → 3 → 4, then 5 and 6 in either order. Step 2 before 4 because
the editor must preserve unknown components before 106's components leave
lib (or it cannot open a 106 scene at all).

**The 3D examples are not in this pass.** `World3d` (`lib/Scene3d.rae`) and
the 3D examples keep their shape for now; they are the same refactor later —
`Transform3dSystem` as the sibling of `Transform2dSystem`, the render/camera
systems owning their derived tables — once the UI side has proven it.

## 7. Decisions (taken 2026-09-21)

- Systems own their tables; `mod`/`view` in signatures says who writes and
  who reads. The App is the aggregate; no `MusicWorld`/`EditorWorld`.
- Aggregation is hand-written (`apply…Components`, `sweep…Entities`,
  snapshot) — three short functions per app. `fields()` one-level recursion
  comes after the shape is proven.
- The lib splits the same way, derived tables first (`LayoutSystem`,
  `ButtonSystem`, `Transform2dSystem`, …); authored shared tables stay on
  `UiWorld`.
- `HeroTransition` stays generic in lib.
- Unknown components in the editor are preserved (the undrained outbox).
- Rounded covers: authored `CornerRadius` + 106's `AlbumCoverSystem`.
- Named component sets are **bundles**, expanded at load, invisible to
  systems, kept by name for round-trip.
