# UI + ECS refactor — status and remaining work

**Status:** assessment, 2026-09-13, from the tree and the queue (not from
memory). Question answered: after ~a week of ECS refactoring across the
compiler, the examples and the two game prototypes, has the same work reached
the flagship UI example (106) and the `.raescene`-driven UI system in
`lib/ui`, and what is still left — everywhere, and in 106 specifically?

## 1. What the wave finished (evidence)

- `lib/ecs` extracted from the UI's own ECS and **`lib/ui` is built on it**:
  `UiWorld` embeds the `World` core (generational `EntityId`, recycling,
  `ComponentTable`, `Hierarchy` + `HierarchySystem`, `Queue`). Not a parallel
  implementation.
- Landed in `lib/ecs`: `query2..5` + the `#807` query-loop sugar,
  `without`/`changedSince` filters, `Tag`, `EventQueue`, `Schedule` +
  `shouldRun`, `Registry` + `Serialize`, `TransformSystem`/`PrevTransformSystem`,
  stable dense-iteration contract (#810), element-alias borrow check (#814).
- Reflection: `fields(world)` + `fieldName` (#772/#773) drive clear/serialize;
  `typeName(T)` (#809) landed; world *construction* by reflection assessed and
  declined (#774).
- No-globals hard error; `lib/ui`'s theme and every example-app global moved
  onto `UiWorld`/`App` resources (#764, #789). Resources follow
  `docs/ecs-resources.md` (plain fields on the world) — `UiWorld.theme`,
  `msdfState`, `nodeIds`, `layoutW/H` already conform.
- 3D examples migrated onto `World3d` + the shared renderer; `Scene3d`
  deleted (#793–#797). Naming/namespace renames, canonical `rae format`.
- 106 was touched heavily (≈420 file-touches in 10 days) — but for the 2D
  renderer C→Rae slices (#907–#920), globals, renames and formatting. **Its
  application shape was not refactored.**
- The games' HUDs already run on `UiWorld` + `.raescene` (112, 114, both
  prototypes), so the shared-UI-system premise holds: fixes in `lib/ui` flow to
  every game.
- The queue has **no open ECS/UI refactor task**. Everything below is
  unqueued.

## 2. Remaining — the UI library (`lib/ui`)

Ordered by leverage.

1. **No `Schedule`.** `lib/ui` and every UI example call systems in hand
   order (106: `FramePipeline.rae`). `lib/ecs/Schedule` exists but has zero
   users in `lib/ui`. Adopt it with **declared read/write tables per system**
   (the architecture doc: "the schedule is the program"; also the
   prerequisite of `docs/parallelism-first-plan.md` Phase 2).
2. **Consumers never moved to the query sugar.** `query2/3`: 0 uses in
   `lib/ui`, 0 in 106. `componentHas(`: 169 in `lib/ui` (SceneLoader 26,
   RenderSystem 20, LayoutSystem 17, legacyRaylib 18). Optional-component
   checks are legitimate; the join-shaped ladders are what step 3 of the plan
   meant to delete and did not.
3. **`.raescene` registry: silent no-op is a correctness bug.** 39 hand-written
   `if name is` dispatch arms (`RegistryDeser.rae` 777 lines); 66 registered
   names vs **76** `UiWorld` tables; unhandled components ("most effects,
   DataRequest, ListView") load as a silent no-op. For a top-notch scene
   format this must be a diagnostic (`scene X node Y: component Z is not
   supported / not registered`), and the arm-per-component boilerplate is the
   remaining wishlist item — `typeName` landed for exactly this and has 0
   uses.
4. **Events.** `EventQueue(T)` exists; the UI input path hand-rolls an
   `ActionEvent` list (`appendActionEvent`/`clearActionEvents`). Migrate to
   `EventQueue(UiAction)` drained by systems in schedule order.
5. **Observation.** The docs prescribe `changedSince`/`componentModStamp`; the
   UI examples use bespoke revision ints (`historyLen`, `sheetEpoch`,
   `playbackRevision`). Migrate.
6. **`ListView` / virtualisation** is a component type with no system; 106
   hand-rolls history windowing per view. A real `listViewSystem` is required
   before any editor/DAW list (tracks, clips, albums).
7. **Editor/DAW capability gaps** (scan): text input 0, clipboard 0, keyboard
   handling 0, undo 1, focus 5 (weak); drag/dock/split exist in some form.
   Needs a design doc before the DAW: focus + keyboard routing, text editing,
   shortcuts, clipboard, undo stack, drag-and-drop, docking.
8. **Dead code + stale docs.** `lib/ui/legacyRaylib` (13 files, 2 near the
   cap) is used only by `examples/legacy` — move or delete. `lib/ui/Theme.rae`'s
   header still describes a module-level active theme (pre-#764).
   `docs/ui-theme-system.md` says "PROPOSED, no implementation" although the
   theme resource exists. `docs/concurrency-model.md` §5 is stale.
9. **File caps:** `Components.rae` 879, `RenderSystem.rae` 830,
   `RegistryDeser.rae` 777. Registry synthesis (item 3) is what shrinks the
   last one.

## 3. Remaining — 106 specifically

1. **`Main.rae` is one 958-line `main` function** (cap 1000; the formatter
   refuses to write over-cap files). Split into `App`/`AppTypes`/`AppCreate`
   plus the loop, as the games did.
2. **Flat layout, 44 files in one folder.** Move to the folder-per-system
   layout the prototypes settled on (`inputSystem/`, `historySystem/`,
   `playbackSystem/`, `screenSystem/`, …).
3. **16 `if screen is` ladders** in `ScreenRouter.rae` (`buildScreenWorld`,
   `pageIdForScreen`, `scrollBoundsForScreen`, …) — the exact query-by-tag
   pattern the architecture doc removed from 112's animation. Screens become
   page entities with a `ScreenPage` component + tags; the router becomes a
   table.
4. **Hand-rolled frame sequencing** (`runFrameInputDispatch` →
   `runFrameLayoutTransform` → `runFrameAnimation` → render) → `Schedule`
   entries with `shouldRun`.
5. `ActionEvent` → `EventQueue`; `UiRefreshCache` revisions → `changedSince`.
6. History windowing → `lib/ui` `ListView` (item 2.6).
7. Background I/O (Spotify poller, artwork curl) → `spawn` + `Channel` + an
   event-loop `wake()` (parallelism plan §5).
8. `DESIGN.md` is raylib-era (texture handles, coordinate space, Live target
   as open questions) — refresh or retire.

104/105 are fine as they are (one file each, no ladders) but should be the
first to show the `Schedule` form once it exists — they are the teaching
examples.

## 4. Suggested order

1. `lib/ui` on `Schedule` with declared sets (unblocks parallelism too).
2. Registry diagnostic for unsupported components + `typeName`-keyed
   synthesis (correctness of `.raescene`).
3. `EventQueue` + `changedSince` in `lib/ui`, then 106.
4. 106 structural split (`Main.rae`, folder-per-system, screen ladder → tags).
5. `listViewSystem`.
6. Editor-capability design doc (focus/keyboard/text/clipboard/undo/dock),
   gated on the DAW plan.
7. Delete `legacyRaylib`, refresh the stale docs.
