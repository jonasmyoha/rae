# 106 — the reference UI-in-ECS example

`examples/106_mobile_ui` is the worked example of **how a whole application —
not a game — is built in Rae's ECS**: a music-player UI with seven screens,
live lists, a search field, a bottom sheet, a hero (shared-element) transition,
optional Spotify mirroring, hot-reload and persistence. Every page is an
authored `.raescene`; everything the scene cannot express (a live count, a data
list, the playing track, a scroll position) is a **component** on a node, driven
by **one system per binding kind**. There is no per-screen imperative UI code.

This document is the map: the resources, the tables, the frame loop, the full
component→system table, and the recipe for adding a page. It is the companion to
the architecture rationale in `docs/ecs-general-architecture.md` and the
frame-phase model in `docs/ecs-systems-and-data-observation.md`; the app's own
`examples/106_mobile_ui/DESIGN.md` is the file-level tour.

---

## 1. The shape in one paragraph

One `App` owns the world-independent resources; one `AppSession` owns the
interactive-run state that outlives a world rebuild. The seven screens are rows
of a `ScreenPage` table, each mounted by the single `mountScreenPage` from its
authored scene. What a tap *means* is a row of the `ActionTable`; the input
phase turns a fired id into a typed `AppCommand` on an `EventQueue` that the
command systems drain. What the scene *shows* from app state is a set of
binding components (`ListView`+`ListSource`, `TextBinding`, `PlaybackIcon`,
`ScrollRoot`, `HasHistory`, …), each written by one system registered on an
observation `Schedule` with the data sources it reads, so an idle frame runs
zero of them. The loop body is four frame systems. No page is found by walking
node ids, no screen is chosen by an `if screen is …` ladder, and no page is
unmounted to refresh its data (§7).

---

## 2. Resources — `App` and `AppSession`

Two records, by lifetime (`AppTypes.rae`, `AppSession.rae`; the pattern is
`docs/ecs-resources.md`).

**`App`** — built once by `createApp`, outlives everything: the GPU/window
handles, the loaded `AlbumScenes`, the `TextureRegistry`, `AppState` (theme,
media library, browsed album/track, tab + sub-page stack, play history,
liked/playlists, bottom sheet, loader cursors), `PlaybackState`, the `Viewport`,
the `SpotifyState` + its poll worker, and — the two that make actions data —
`actions: ActionTable` and `commands: EventQueue(AppCommand)`.

**`AppSession`** — created once per interactive run, survives every world
rebuild: the route (`currentScreen`), the hot-reload watchers, the debug UI,
the scroll FSM + `ScrollMemory` (page id → scroll position), the input system,
the active-album cache, and the render/paint pacing fields. The hero transition
is **not** here any more — it is a world entity (§5).

`AppState` lives in `screenSystem/AppState.rae`; the `Screen` enum and the
screen table live in `screenSystem/ScreenRouter.rae`.

---

## 3. The frame loop

`runApp` (`App.rae`, ~40 lines) is two nested loops: the outer rebuilds the
persistent world (`buildSessionWorld` + `createWorldFrame`, `WorldBuildSystem.rae`)
when something structural changed; the inner runs four frame systems per frame
(`frameSystem/`), each taking `app` / `session` / `world` / `frame` / `tick`:

```
frameWaitSystem    phase 0 — wait policy: busy-render while interacting /
                   animating / a hero transition is in flight, blocking
                   event-wait when idle; writes the scroll bounds + activity
                   flags into the tick, then waits.
frameInputSystem   phase 1 — pollReload, gather input, live-resize probe, the
                   scroll FSM, then observation pass A: the action table turns
                   fired ids into commands, the command systems run, and
                   runAppObservation writes every binding whose source moved.
frameUpdateSystem  phase 2 — data sync (playback clock, Spotify mirror, asset
                   loaders) + observation pass B, the hero transition step,
                   layout/fit/transform (schedule-gated), the render decision.
frameRenderSystem  phase 3 — paint (dirty-gated), then handleScreenSwitch:
                   toggle page Active, re-run layout for the first frame there,
                   spawn the hero transition.
```

The lib/ui systems (layout, transform, fit, visualBounds, widgetStyle,
hoverScale, render) are gated by a lib `Schedule` (`createUiPipeline`,
`uiShouldRun`) that skips a system whose read tables did not move — see
`docs/ui-ecs-refactor-status.md` §1. Rae has no function references, so the app
still *calls* each system in order; the schedule owns the declarations and the
dirty state.

---

## 4. Actions as a table, navigation as commands

`actionSystem/ActionTable.rae` maps every id a scene, a list row or the debug
menu can fire to an `ActionKind` + payload. A **template** row parses its
payload once in the lookup (`album.open.{stem}`, `history.replay.{index}`,
`song.openref.{index}.{stem}`); the sheet menus are rows too. The input phase
(`actionCommandSystem`) turns each fired `OnClick.actionId` into a typed
`AppCommand` on `App.commands` (a lib/ecs `EventQueue`); the command systems in
`ActionSystems.rae` consume kinds, never strings:

- `playbackCommandSystem` — toggle / next / prev / playTrack / replayHistory /
  toggleLike (feeds `PlaybackState`, the history, the liked playlist, Spotify).
- `sheetCommandSystem` — open / close / libraryRemove (sends a `pop`).
- `canvasCommandSystem` — the canvas overlay flag.
- `navigationSystem` — the ONLY writer of `AppState.tab`, the sub-page stack and
  the browsed collection; runs last so a same-frame `pop` is consumed.

`actionSystem/ActionAudit.rae` walks every `OnClick` / `ActionBinding` /
list-binding prefix / sheet-menu id in the built world and asserts a table row
exists — the 106 gate fails on an unknown authored id before a user ever taps it.

---

## 5. The component → system table

Every component authored across the 106 scenes, and what drives it. **Layout /
paint vocabulary** is the lib/ui pipeline (layout → fit → transform →
visualBounds → render); it needs no app code. **Binding components** each have
one system; the app systems marked *(obs)* are rows of the observation schedule
(`observeSystem/AppObservation.rae`) and run only when a declared source moved.

### Layout / paint vocabulary (lib/ui pipeline — no app system)

| component | driven by |
|---|---|
| `Rect` `Size` `Layout` `Padding` `Align` `Offset` `Aspect` `ScaleToFit` | `layoutSystem` / `fitSystem` |
| `Sprite` `Text` `Shape` `CornerRadius` `GradientFill` `Opacity` | `renderSystem` |
| `WorldTransform2D` (derived) `ExtentAnchor` | `transformSystem` |
| `SafeArea` | `safeAreaSystem` (app calls `applySafeAreas`) |
| `AnchorBottom` `LayerTag` | `anchorBottomSystem` / `layerTagSystem` (app, DockSystems) |
| `HoverScale` | `hoverScaleSystem` |
| `PointerEvents` `OnClick` `Active` | input system + render visibility |
| `WidgetId` `WidgetState` `StateStyle` | `widgetStyleSystem` |
| `SceneInstance` | resolved at mount (`resolveSceneInstances`) |

### Binding components (one system each)

| component | system | where |
|---|---|---|
| `ListView` | `listViewSystem` | lib/ui *(materialises rows)* |
| `ListSource` | `listSourceSystem` *(obs)* | `listSystem/ListSources.rae` |
| `TextBinding` (`spotify.*`) | `spotifyBindingSystem` | `spotifySystem/SpotifyView.rae` |
| `TextBinding` (`album.*`) | `albumHeaderSystem` *(obs)* | `screenSystem/AlbumSystems.rae` |
| `AlbumHeader` (cover marker) | `albumHeaderSystem` *(obs)* | `screenSystem/AlbumSystems.rae` |
| `TrackText` | `trackTextSystem` *(obs)* | `playbackSystem/PlaybackBindings.rae` |
| `PlaybackTime` | `playbackTimeSystem` *(obs)* | `playbackSystem/PlaybackBindings.rae` |
| `PlaybackIcon` | `playbackIconSystem` *(obs)* | `playbackSystem/PlaybackBindings.rae` |
| `PlaybackCover` | `playbackCoverSystem` *(obs)* | `playbackSystem/PlaybackBindings.rae` |
| `LikedHeart` | `likedHeartSystem` *(obs)* | `playbackSystem/PlaybackBindings.rae` |
| `ProgressBar` | `applyPlaybackToWorld` *(obs: "progress")* | `playbackSystem/Playback.rae` |
| `ProfileStat` | `profileStatsSystem` *(obs)* | `screenSystem/ProfileView.rae` |
| `AvatarSource` | `avatarSystem` *(obs)* | `screenSystem/ProfileView.rae` |
| `HistoryList` | `historyListSystem` *(obs)* | `historySystem/HistoryView.rae` |
| `HasHistory` | `hasHistorySystem` *(obs)* | `screenSystem/HomeView.rae` |
| `EmptyState` | `emptyStateSystem` *(obs)* | `screenSystem/EmptyStateSystem.rae` |
| `AlbumCover` (+ authored `CornerRadius`) | `albumCoverUpdate` *(obs)* | albumCoverSystem/AlbumCoverSystem — the app's own table (ECS split step 3): the rounding is scene data, the setting removes / restores it |
| `BottomSheet` / `SheetPanel` | `bottomSheetSystem` *(obs)* | `screenSystem/BottomSheet.rae` |
| `SearchField` | `searchFieldSystem` | `screenSystem/SearchSystems.rae` (input phase) |
| `NavTab` | `navTabsSystem` | `screenSystem/DockSystems.rae` (on switch) |
| `ScrollRoot` | `scrollRootSystem` + the scroll FSM | lib/ui/scrollRootSystem + `inputSystem/ScrollInput.rae` |
| `HeroWidget` | `spawnHeroTransition` / `heroTransitionSystem` | lib/ui/animationSystem (key written by the cover binding systems) |

### Who OWNS which table (ECS split step 4, docs/ecs-systems-own-their-tables.md)

The app-specific components above are no longer `UiWorld` tables: each is a
`ComponentTable` field on the system that reads it, and the systems are the
fields of `MusicSystems` (`MusicSystems.rae`) on the `App` — the hand-written
aggregate the two outbox drains (`applyMusicComponents`, `sweepMusicEntities`,
run by `musicFrameEnd`) and the observation walk. A route rebuild resets the
tables (`resetMusicTables`); the playback state itself survives it.

| system (type) | owns | absorbed state |
|---|---|---|
| `PlaybackSystem` | playbackIcons, playbackCovers, likedHearts, trackTexts, playbackTimes | the former `PlaybackState` (clock, playing track, inbox) |
| `DockSystem` | navTabs | — |
| `AlbumSystem` | albumHeaders | — |
| `ProfileSystem` | profileStats, avatarSources | — |
| `HistorySystem` | historyLists | — (`PlayHistory` / `HistoryArtLoader` stay on `AppState`: they are app data read everywhere, not the list binding's) |
| `HomeSystem` | hasHistories | — |
| `AlbumCoverSystem` | albumCovers, authoredRadii | — |
| `SpotifySystem` | (no table) | the former `SpotifyState`; `SpotifyPoller` is its worker |

The observers judge "did my input move" from the world tables they still
declare, their sources, AND their own table's generation
(`ownTableGeneration`), so a row a ListView mounts — drained into the system
at frame end — runs the observer next frame. Every update function takes its
system as the camelCase of its type, `mod` when it writes it, `view` when it
only reads it.

### Authored metadata (read at mount, no per-frame system)

`ActionBinding` (id + role — the audit and the debug menu read it),
`DataDependency` (revision-observer metadata).

---

## 6. How to add a page

A new page is **its scene + its table row + (if it shows app data) a producer**.
No new imperative screen code.

1. **Author the scene** `assets/scenes/Foo.raescene`: a node tree with a
   `SafeArea`/`ExtentAnchor` on the root, a `ScrollRoot` on the scrolling body,
   and — for anything live — a binding component: a `ListView` + `ListSource`
   for a list, a `TextBinding`/`ProfileStat`/… for a value, an `OnClick` whose
   id is in the action table.
2. **Register the scene** in `loadPageScenes` (`screenSystem/SceneLoader.rae`),
   keyed by the page id, and add its path constant in `Config.rae`.
3. **Add the table row** in `createScreenPages` (`ScreenRouter.rae`):
   `ScreenPage { screen, pageId, label, refreshOnEntry }`. Add the `Screen`
   enum case; `mountScreenPage` and `buildAppWorldFor` pick it up automatically.
4. **If it shows a data list**, add a `ListSourceKind` case + one
   `ListViewData` producer in `listSystem/ListSources.rae` and wire it in
   `listViewDataFor` / `listSourceKey`. That producer is the only new app code.
5. **If it needs a new binding kind**, add the component (lib/ui
   `BindingComponents.rae` + its table in `Ecs.rae`) and one system, then
   register the system as an observation row with its `source*` flags.

---

## 7. Measurements (recorded 2026-09-20)

The invariants this example is meant to demonstrate, measured on the tree at the
time of writing. The gate greps are cheap and run in the 106 example gate.

**No node-id pokes outside the mounter.**
`grep -rn findNodeInPage examples/106_mobile_ui --include='*.rae' | grep -v SceneMount`
= **0**. Every live value reaches its node through a component the driving
system iterates, not a page + node-id lookup.

**No screen ladders outside the persistence map.**
`grep -rn 'is Screen\.' examples/106_mobile_ui --include='*.rae' | grep -v HotReloadGlue`
= **0**. The `Screen` enum is matched only where an enum belongs — the
save-state ordinal map in `HotReloadGlue.rae`.

**Idle frame runs zero app systems.** A paused headless run's last `[loop]`
line reports `appSystems=0` (asserted by the 106 gate); the observation schedule
skips every binding whose sources are unchanged.

**A page is its scene plus a small producer.** App code per page, measured as
the page's `ListViewData` producer plus any page-specific binding system (code
lines, comments/blanks excluded):

| page | scene (lines) | data producer | + page system |
|---|---|---|---|
| Home | 308 | `coverGridData` 14, `recentData` 29 | `hasHistorySystem` 17 |
| Library | 229 | (shares `coverGridData`) | `collectLibraryCoverKeys` 18 |
| Album | 553 | `albumTracksData` 10 | `albumHeaderSystem` 44 |
| Search | 382 | `searchData` 47 | `searchFieldSystem` 61 |
| History | 148 | `historyListData` 29 | `historyListSystem` 20 |
| Profile | 829 | — | `profileStatsSystem` 21, `avatarSystem` 30 |
| Player | 507 | — (pure playback bindings) | — |

The pure data producer is 10–47 lines; the Now Playing page has **no** app code
at all (it is scene + shared playback bindings). Search and Album carry a larger
per-frame system (text input; the header + row highlight) beyond the producer —
those are the honest upper bound, not 30.

**Zero unmount-to-refresh.** No page is unmounted to refresh its data. Lists
rebind in place through `ListSource`; a browsed **stem or track change** — the
album track-row highlight and the player header — is a component write the
observation pass picks up in place (the album `TrackList` source keys on the
playing stem|index; the player is `PlaybackCover`/`TrackText`/`PlaybackTime`),
with the album's `ScrollRoot` reset to the top on a new album
(`resetScrollRootOnPage`). The only `unmountPage`/`mountScreenPage` left in
`ScreenSwitch.rae` is the `ScreenPage.refreshOnEntry` path — a deliberate
re-mount hook for a page that wants fresh mount on entry, false for every page
today (so it never fires).

---

## 8. Related docs

- `docs/ecs-general-architecture.md` — why ECS for a UI, the binding pattern.
- `docs/ecs-systems-and-data-observation.md` — the frame phases + observation.
- `docs/ui-ecs-refactor-status.md` — the wave that produced this shape.
- `examples/106_mobile_ui/DESIGN.md` — the file-by-file tour of the app.
