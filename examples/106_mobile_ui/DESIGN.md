# 106_mobile_ui — Design

Status: **the reference UI application for Rae** (refreshed 2026-09-14 after
the #946–#950 refactor; docs/ui-ecs-refactor-status.md §3 is the ledger of
what changed and why). It is a music-player mockup — Home, Search, Library,
Profile, Album, Now Playing, History, a bottom sheet and a debug menu —
driven by the local Spotify desktop app on macOS and a local album catalogue
everywhere else.

What this document is: the shape of the *app* — how a Rae program is
organised around `lib/ecs` + `lib/ui`, what the frame does, who owns what.
What it is not: a restatement of the library. Every `lib/ui` mechanism the
app uses has its own document; this one links them. The earlier version of
this file was the 98-era *plan* for building `lib/ui` on raylib; that plan
shipped, the library moved on, and the plan's open questions are answered in
§9. The plan itself is in git history if you need it.

## 1. What runs where

```
examples/106_mobile_ui/            the app: ~53 files, one folder per system
  Main.rae                         main() = createApp() + runApp(app)
  AppTypes.rae / AppCreate.rae / App.rae
  FramePipeline.rae                the named frame phases (§4)
  Config, Viewport, WorldHelpers, UiRefresh, RenderDecide, RenderCacheGpu2d,
  GpuWindow, Headless, HotReloadGlue, AppSettings, FileIo
  inputSystem/ screenSystem/ historySystem/ playbackSystem/ spotifySystem/
  debugSystem/ assetSystem/
  assets/scenes/*.raescene         12 authored scenes + Theme.raescene
  assets/                          MSDF fonts, icons, album art, catalogue
  app_cache/                       runtime state (gitignored): history,
                                   library, playlists, downloaded artwork

lib/ui/                            the UI library the app is built on
lib/ecs/                           the ECS core lib/ui is built on
lib/Gpu2d*.rae, lib/webgpu/        the 2D renderer: Rae over WebGPU bindings
lib/sys/Spotify.rae                the osascript/curl C ABI (no scheduling)
```

The rule the split follows: **library = mechanism, app = policy.** Layout,
transforms, scene loading, the theme, input hit-testing, rendering, list
virtualisation, the schedule — mechanism, in `lib/ui`, reused unchanged by
104/105/111/112/114 and the game prototypes. Which screens exist, what a
tap does, what Spotify's state means for the play button, what is persisted
— policy, here.

## 2. The five layers, as they are now

```
┌──────────────────────────────────────────────┐
│ 1. ECS storage                               │  lib/ecs: World, EntityId
│    UiWorld = World core + one named           │  (generational), Component-
│    ComponentTable per UI component            │  Table, Hierarchy, query2..5,
│                                               │  EventQueue, Schedule
├──────────────────────────────────────────────┤
│ 2. Scene format + loader + theme              │  lib/ui/Scene*, SceneInstance,
│    .raescene JSON -> components via the       │  Registry*, Theme*, Frames
│    registry; theme tokens resolved per world  │
├──────────────────────────────────────────────┤
│ 3. Layout + transform                         │  lib/ui/layoutSystem, fitSystem,
│    safe area -> layout -> fit -> world        │  safeAreaSystem, Transform,
│    transform -> visual bounds                 │  visualBoundsSystem, Pipeline
├──────────────────────────────────────────────┤
│ 4. Render                                     │  lib/ui/renderSystem over
│    retained gpu2d canvas: boxes, MSDF text,   │  lib/Gpu2dCanvas + lib/webgpu
│    images, layers; presented through SDL3     │
├──────────────────────────────────────────────┤
│ 5. Behaviours                                 │  lib/ui/inputSystem, animation-
│    input/hit-test/actions, animation, scroll, │  System, ScrollPhysics/Panel,
│    widget style, list view, overlays          │  widgetStyleSystem, listView-
│                                               │  System, LogOverlay/DebugOverlay
└──────────────────────────────────────────────┘
```

Two things changed since the plan and are worth knowing before reading code:

- **The ECS is `lib/ecs`, not a UI-private one.** `UiWorld` embeds the
  generic `World` (generational ids with recycling, sparse-set tables, a
  `Hierarchy`), and the UI systems are ordinary systems over it. Reference:
  `docs/ecs-api-reference.md`, `docs/ecs-general-architecture.md`,
  `docs/ecs-systems-and-data-observation.md`.
- **The renderer is retained, not immediate.** gpu2d keeps a canvas of box /
  text / image passes on the GPU and the render system re-records only when
  its inputs changed; the app decides per frame whether a repaint is needed
  at all (`RenderDecide.rae`). Reference: `docs/webgpu-2d-ui-renderer.md`,
  `docs/ui-render-loop-performance.md`.

## 3. The app object

`type App` (AppTypes.rae) is the program's long-lived state, built once by
`createApp()` (AppCreate.rae: window, fonts, scenes, catalogue, persisted
state, Spotify) and run by `runApp(app: mod App)` (App.rae: the interactive
loop and teardown). Its fields are the setup-time singletons — `gpuUi`,
`scenes`, `texReg`, `appState`, `playback`, `viewport`, `logView`,
`spotifyState` + `spotifyPoller`, the screen table — and *nothing* is a
module-level global: every singleton is a **resource on `App` or `AppState`**
threaded through the `mod`/`view` parameter that says who may touch it
(`docs/globals-and-app-ownership.md`, `docs/ecs-resources.md`).

`AppState` (screenSystem/ScreenRouter.rae) is the persisted + navigational
state: the theme, the media library, the current album/track, the tab and
sub-page stack, the play history, liked/playlists, the bottom sheet, and the
transient loader cursors (`historyArt`, `artworkFetch`, `assetLoad`). It is
saved to `app_cache/` on every action and at exit, restored at boot.

One `UiWorld` holds every screen at once. Screens are **page entities**: a
`ScreenPage` table (`createScreenPages()`) lists each screen's page id and
scroll policy as data, the page root is tagged `PageRoot{id}`, and switching
screens toggles `Active` on the pages (`buildAppWorldFor(visible)`). There
are no per-screen world builders and no `if screen is` ladders in the router.

## 4. The frame

`App.rae` owns the loop; `FramePipeline.rae` names its phases. Per iteration:

```
waitEvents(timeout)          hybrid loop: 0 while animating/interacting,
                             nextWaitTimeoutSec(...) when idle (§5)
pollClose + gather input     lib/ui inputSystem over the world
applyWheelScroll             scroll FSM (inputSystem/ScrollInput.rae)
1. runFrameInputDispatch     hit-test, uiActions EventQueue -> action handlers
                             mutate AppState / issue commands; may pick a
                             next screen
2. processCommands(playback) Apply(A): the PlaybackSystem drains its inbox
3. refreshUiDiffs +          observation: revision ints on resources
   syncFrameData             (history, playback, sheet) -> component edits;
                             Spotify tick -> mirror; artwork/asset loaders;
                             the History window re-rows in place on scroll
4. runFrameLayoutTransform   safeArea -> layout -> fit -> transform ->
                             visualBounds, each gated by uiShouldRun on the
                             world's Schedule (dirty tables skip)
5. runFrameAnimation         hero transition, hover scale, scroll spring
6. decideActive + render     RenderDecide: did anything change? if so
                             renderGpu2dFrame records + presents the canvas
7. handleScreenSwitch        after the frame, only when input picked a new
                             screen: toggle pages, hero capture, re-run
                             layout/transform for the first frame there
```

The mapping onto the ECS phase list in
`docs/ecs-systems-and-data-observation.md` §4 (Input → Apply → Observe →
Layout/Transform → Render) is written in a comment right above the loop
body. Two design points:

- **The app calls the systems; the Schedule owns the dirty state.** Rae has
  no function references, so the schedule cannot call systems for you.
  `createUiPipeline(world)` registers every lib/ui system with its declared
  read/write tables (`lib/ui/Pipeline.rae`), and each call site asks
  `uiShouldRun(schedule, index)` first. Layout and transform skip when their
  inputs are unchanged; fixture 839 proves that judgement equals the hand
  caches it replaced (#949).
- **Observation is by revision, not by diffing.** Resource-shaped state
  (`PlayHistory.revision`, `PlaybackState` revision, the sheet epoch) bumps
  an Int on change and `UiRefreshCache` remembers the last one it reacted
  to; component tables use `changedSince`. Both are §7 of the observation
  doc; 106 uses each where it belongs (#944).

## 5. The event loop

Idle at ~0 % CPU, smooth while moving: `waitEvents` blocks for a policy
timeout (`lib/ui/EventLoop.nextWaitTimeoutSec`: a frame while animating or
with the mouse down, the watcher poll cadence otherwise, a 30 s cap), and
busy-renders (timeout 0) during interaction, transitions and scroll
momentum. Every continuous-motion source must feed the "animating" flag or
it silently degrades to the watcher rate — see
`docs/ui-render-loop-performance.md` for the postmortem behind this.

Background work does not poll from the loop: a worker that finishes calls
`EventLoop.wake()` (a thread-safe SDL user event, #950) so a parked
`waitEvents` returns at once.

## 6. Background I/O: spawn + Channel + wake

Everything the app *waits* on runs on Rae workers, not runtime threads
(`docs/parallelism-first-plan.md` §5):

- `spotifySystem/SpotifyPoller.rae` — one `spawn`'d worker: `Spotify.refresh()`
  (an osascript fork/exec) → a tick on a `Channel(Int)` → `wake()` → 50 ms
  stop-checked sleep slices. The frame's `pollSpotify` mirrors the cache
  into `PlaybackState` and the Now Playing texts only when a tick arrived.
  A stop channel + `task.get()` at teardown stand in for `detach`.
- `assetSystem/ArtworkFetch.rae` — one `spawn`'d worker per album-art
  download (`Spotify.fetchArtwork`, a blocking atomic curl), posting an
  `ArtworkResult { serial, ok }` on one `Channel(ArtworkResult)` (#969);
  `artworkFetchDrain` settles results once per frame and the History loader
  (`historySystem/HistoryIo.rae`) uploads textures as they land.
- `lib/sys/Spotify.rae` is only the C ABI: refresh, the mutex-guarded cache
  getters, the transport controls, the blocking fetch.

## 7. Scenes, registry, theme

Screens are authored in `assets/scenes/*.raescene` — the RUICS JSON format
(`docs/game-proto1-ruics-ecs-reference.md`): a node tree where each node
carries named components (`Rect`, `Layout`, `Text`, `Sprite`, `OnClick`,
`ScrollPanel`, `ListView`, …), with `SceneInstance` for reuse (the nav tabs,
the mini-player, a track row) and per-instance overrides. `lib/ui/Scene*`
parse and mount; `lib/ui/Registry*` map a component name to its
deserialiser; an unknown or unsupported component is a fatal diagnostic
naming the scene and node (#941), never a silent no-op. `Theme.raescene`
declares the tokens (colours, spacing, text styles) that the loader
resolves through the world's theme resource (`docs/ui-theme-system.md`);
`RAE_UI_THEME` picks a variant at boot. The registry is still one arm per
component; #959–#961 collapse it via reflection.

The app-side glue is small: `screenSystem/SceneLoader.rae` loads the scene
set, `SceneMount.rae` turns a failed mount into a visible red sentinel, the
`*View.rae` files mount a page and bind its data (album rows, history rows,
the playback overlays), `WorldHelpers.rae` shortens the component setters.

## 8. Coordinates, viewport, assets, persistence, headless

- **Coordinates:** Model A of `docs/ui-coordinate-and-responsive-layout.md`
  — scenes are authored in design units (a 1080-wide phone frame,
  `unitScale=3`), gpu2d's design-resolution transform fits the frame into
  the SDL3 window (letterboxed), and `Viewport.rae` is the one place that
  converts. Safe areas come from `docs/ui-viewport-and-safe-area-plan.md`;
  `RAE_UI_DEVICE` / `RAE_UI_FRAME` pick device presets.
- **Text:** MSDF atlases (`assets/*.mtsdf.*`, `lib/SdfText`, `lib/ui/MsdfState`)
  — no raylib fonts, no per-glyph textures.
- **Images:** `assetSystem/GpuAssetRegistry.rae` owns the gpu2d canvas and
  its image keys; covers/avatars load progressively (`cascadeAssets`,
  budgeted per frame) and the History thumbs through §6.
- **Persistence:** JSON under `app_cache/` (`PlaybackStateIo`, `HistoryIo`,
  `LibraryData`, `PlaylistData`), all via the one `FileIo.rae` declaration
  pair. `assets/` is authored input; `app_cache/` is runtime output.
- **Hot reload:** `.raescene`/theme edits reload live through
  `lib/FileWatch` (`HotReloadGlue.rae` preserves navigation across a
  rebuild).
- **Headless + automation:** `RAE_UI_HEADLESS=1` (no focus steal, Spotify
  off), `RAE_AUTO_EXIT_SEC`, `RAE_UI_SCREEN/SCROLL/PLAYING/PROGRESS` to pose
  a state for a screenshot, `RAE_UI_STRESS_*` for the rebuild stress runner,
  `RAE_UI_DEBUG_LOOP` / `RAE_UI_WAIT_TRACE` for loop diagnostics. The
  example gate (`RAE_EXAMPLE_FILTER=106_mobile_ui make test-examples`) runs
  the app headless and screenshots it.

## 9. The old open questions, answered

| 98-era question | Answer in the tree |
|---|---|
| Where do raylib `Texture` handles live? | There are none. gpu2d owns images by string key on its canvas; `TextureRegistry` (assetSystem/GpuAssetRegistry.rae) is the app's handle. |
| Pixel coordinates, origin top-left, no translation? | Design units, not pixels: Model A with a design-resolution fit transform and a `Viewport` spine (§8). |
| HiDPI at 1×, let raylib scale? | The window is sized in logical points, the framebuffer in physical px; `dpr` is part of `Viewport`, and MSDF text is resolution-independent. |
| Data hot reload of `.raescene`? | Landed (`lib/FileWatch` + `HotReloadGlue.rae`). |
| List virtualisation cutover? | `lib/ui/listViewSystem` exists (#945); the History list still hand-windows its rows — the migration is #964. |
| The Live (VM) target? | Removed (#957). Compiled is the one target; `spawn`/`Channel` are real threads. |
| raylib immediate-mode painter, no `RenderSyncSystem`? | Replaced by the retained gpu2d canvas; the render system re-records on change and `RenderDecide` skips frames with nothing to draw. |
| Manual per-component registry — reflection or codegen? | Still manual; the reflection path is queued (#959–#961) on `fields`/`typeName`, which now exist. |

## 10. Cross-references

- `docs/ui-ecs-refactor-status.md` — what the #939–#950 wave changed in
  `lib/ui` and here, and what is still open.
- `docs/ecs-api-reference.md`, `docs/ecs-general-architecture.md`,
  `docs/ecs-systems-and-data-observation.md`, `docs/ecs-resources.md` — the
  ECS the app is built on.
- `docs/webgpu-2d-ui-renderer.md`, `docs/ui-render-loop-performance.md`,
  `docs/event-driven-ui-loop-plan.md` — renderer and loop.
- `docs/ui-coordinate-and-responsive-layout.md`,
  `docs/ui-viewport-and-safe-area-plan.md`, `docs/coordinate-system.md`.
- `docs/ui-theme-system.md`, `docs/game-proto1-ruics-ecs-reference.md` —
  theme and scene format.
- `docs/parallelism-first-plan.md` §5 — why the background I/O is shaped as
  it is.
- `docs/globals-and-app-ownership.md` — why nothing here is a global.
- `examples/104_ui_hello`, `examples/105_ui_counter` — the small teaching examples of the
  same Schedule-driven shape; 111/112/114 and the prototypes for HUDs on the
  same library.
