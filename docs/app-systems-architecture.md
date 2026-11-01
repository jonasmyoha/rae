# App / systems architecture (reference: the walker app shell, `lib/app3d`)

Status: implemented. The walker began as `examples/136_walker_character`
(first numbered 114) and its systems now live in Rae's library as the walker
app shell, `lib/app3d/Walker*.rae`. Examples 136 (the island, deferred), 137
(the island, forward) and 143 (the wide forest, deferred) are each a short
`Main.rae` that calls `runWalkerApp(settings:)` (`app3d/WalkerAppRun`) with
its `WalkerAppSettings` (`app3d/WalkerAppSetup`). The sections below keep the
design as it was planned; "Files" says where each part lives today. For a
project outside this repository with one folder per system
(`xxxSystem/XxxSystem.rae`), see `examples/106_mobile_ui` and
`docs/ecs-systems-own-their-tables.md`.

## Why

`examples/136_walker_character/main.rae` grew to the hard 1000-line cap doing
setup + per-frame orchestration + inline system logic in one function. The cap is
a smell: the app has no structure, so "add a feature" means "make the monolith
bigger." We want a structure that a real game keeps — distinct systems that own
their state and their update/render — so adding a system is one new file and one
call, not a diff to a 1000-line function.

The proven in-repo template is `examples/97_tetris3d`: a state struct + per-system
files (`input.rae`, `physics.rae`, `camera.rae`, `render.rae`, …) + a
fixed-timestep accumulator loop. This plan follows it, scaled up.

## Core shape

`App` owns the systems. Each system owns its own state (and, where it needs one,
its own component table). Systems are functions in their own file with an
`update` and, where they draw, a `render`.

```
type App {
  scene: Scene3d        # shared render world; systems add/keep entities in it
  frame: Frame          # dt, time, aspect, needsRender (per-frame scratch)
  input: InputState
  players: PlayerRoster # player -> character mapping + input source
  characters: CharacterSystem   # a ROSTER of characters, not one hero
  ui: UiState
  camera: CameraState
  terrain: TerrainState
  grass: GrassState
  physics: PhysicsState # fixed-step sim seam (Jolt/Box2D later)
  net: NetState         # multiplayer seam (offline today)
  render: RenderState
  persist: PersistState
  ok: Bool
}

func createApp() ret App          # all setup, split per system
func runWalkerApp() ret Int       # the loop
func updateApp(app: mod App)      # sim/update phase (variable dt + fixed-step physics)
func renderApp(app: mod App)      # render phase (animation sample + render-graph walk)
```

Loop (in `app.rae`):

```
loop not gpu2d.pollClose() {
  frameBegin(app: app)                                    # dt, profile tick, resize
  updateApp(app: app)
  if gpu2d.shouldRenderFrame(appNeedsRender: app.frame.needsRender) {
    renderApp(app: app)
  } else {
    gpu2d.waitEvents(timeoutSec: nextWaitTimeoutSec(...))
  }
}
```

`gpu2d.shouldRenderFrame` folds in the window-visibility gate (never render while
hidden/occluded — see `docs/ui-render-loop-performance.md`), so the app cannot
reintroduce the app-switch memory leak.

## Passing rule: separate references vs whole App

`view` = read-only reference, `mod` = mutable reference. Prefer decoupling.

- **Default — pass the specific systems a function needs as separate references.**
  `f(camera: view CameraState, chars: mod CharacterSystem, dt: copy Float)`. This
  states the dependency, keeps the function testable, and lets a piece graduate to
  `lib/app3d` unchanged. Roughly **five references or fewer → pass them separately.**
- **Pass the whole `App` (`mod`/`view`)** for high-level orchestration
  (`updateApp`, `renderApp`, the render-graph walk) or when a function genuinely
  touches more than ~five systems — threading everything through named params
  there is just noise.
- **Reusable/stdlib systems NEVER take `App`.** They take their own state + explicit
  deps (`updateCameraRig(rig: mod CameraRig, camera: mod Camera3d, …)` — already the
  house style). `App` is an app-layer concept; `lib` must not depend on it.

Systems may take `mod App` and mutate nested paths directly
(`app.characters.controlled = id`) or pass a nested field as a reference to a
stdlib helper (`moveController(controller: mod app.characters.list[i].controller,
…)`) — both verified on the compiled target.

## Characters: a roster, not a hero

Do NOT hardcode a single character. `CharacterSystem` holds shared assets once and
a list of instances; input drives whichever instance is `controlled`.

```
type Character {
  id: Int
  owner: Int                 # PLAYER_LOCAL or a remote player id (multiplayer)
  controller: ThirdPersonController   # world pos, heading, locomotion
  clipIndex: Int  playing: Bool  speed: Float  animTime: Float
  transitionFrom: SkeletonPose  transitionElapsed: Float
  model: Mat4  prevModel: Mat4        # current + previous, for motion vectors
}

type CharacterSystem {
  # shared assets for this archetype (one skeleton/clip set + mesh parts)
  skeleton: Skeleton
  idleClip: Clip  walkClip: Clip  runClip: Clip
  bindPose: SkeletonPose
  partIds: List(Int)  partRough: List(Float)  basePartModel: Mat4
  list: List(Character)      # the roster
  controlled: Int            # id of the character local input drives
}
```

- Each character samples ITS OWN `animTime` -> its own pose -> its own palette ->
  drawn with its own `model`. (Today's "clones share the hero's pose" was a crowd
  optimization; the roster makes them independent. A shared-pose crowd can stay as a
  separate cheap path later, but the default is independent characters.)
- **Switch input to another character:** `characters.controlled = otherId`.
- Multiple archetypes (different skeleton/mesh) later = a list of CharacterSystem, or
  an `archetypeId` on Character indexing shared asset sets. Out of scope now; the
  single-archetype roster is the step that unblocks it.

## Input as intent (multiplayer-ready)

`InputSystem` turns devices into an intent, not directly into movement:

```
type InputIntent { moveX: Float  moveY: Float  run: Bool  actions: Int }
type InputState { ui: UiInput  moveStick: VirtualJoystick  intent: InputIntent }
```

`inputUpdate` fills `intent` from WASD + the touch stick. `characterUpdate` applies
`intent` to `characters.list[controlled]`. This indirection is what makes
multiplayer and input-switching cheap: a remote player's intent arrives from
`NetSystem` instead of a device, and re-targeting local input is one field write.

## Physics + jobs (stubs now)

Following tetris3d's accumulator, physics runs on a FIXED timestep decoupled from
render (deterministic, frame-rate independent):

```
const physicsFixedStep: Float = 1.0 / 60.0
type PhysicsState { accumulator: Float  worldHandle: Int }   # worldHandle -> future Jolt world
func physicsStep(app: mod App, fixedDt: copy Float) { }       # no-op today
```

`updateApp` accumulates `frame.dt` and calls `physicsStep` in a
`loop accumulator >= fixedStep` block. When Jolt/Box2D lands it fills the stub; the
loop does not change.

**Threading seam — `JobSystem`.** Physics, animation sampling and culling are the
parallelisable work. `JobSystem` is the seam: `jobSubmit` queues, `jobAwaitAll`
joins. It runs inline (single-threaded) today; when the runtime worker pool
(`runtime_threads.c`) is wired in, `jobSubmit` dispatches to workers and the systems
above are unchanged.

## Multiplayer seam (stub)

```
type NetState { role: Int  tick: Int }   # role: offline / server / client
func netPublishIntent(net: mod NetState, intent: view InputIntent, characterId: copy Int) { }
func netApplyRemote(net: mod NetState, characters: mod CharacterSystem) { }
```

Offline today. The pieces that make it viable are already in the design:
intent-based movement + a character roster with `owner` + deterministic fixed-step
physics (enables prediction/reconciliation).

## Files (`lib/app3d/`)

One file per system, with a `Walker` prefix, in `lib/app3d/`:

- `WalkerApp.rae` — the `WalkerApp` struct and `createWalkerApp`. Convention
  (#822): the struct IS the system list (`cameraSystem: WalkerCamera`,
  `physicsSystem: WalkerPhysics`, `inputSystem: WalkerInput`, …); each system
  function takes the narrow state it needs, never the whole app.
- `WalkerAppRun.rae` — `runWalkerApp`, the loop. `WalkerAppSetup.rae` —
  `WalkerAppSettings` and setup. `WalkerAppWorld.rae` — the world build.
- `WalkerAppUpdate.rae` — `updateWalkerApp`, a hand-ordered list of system
  calls; systems return small tick structs (`UiTick`, `CameraTick`).
- `WalkerRenderWalk.rae` — `renderWalkerApp`, the render-graph walk.
- Systems: `WalkerInput`, `WalkerCamera`, `WalkerCharacter`, `WalkerTerrain`,
  `WalkerPhysics`, `WalkerRender`, `WalkerUi`, `WalkerNet`, `WalkerMarks`, …
- Self-tests run headless inside the real app: `WalkerSelfTest`,
  `WalkerClimbTest`, `WalkerLayoutTest`, … (env `RAE_WALKER_*_TEST`).
- 143's wide forest: `app3d/WideForest` and `app3d/PropField`.

## stdlib vs app

- **Reused as-is:** CameraRig, ThirdPersonController, RenderScaleController, ControlPanel,
  SettingsState, ui/ecs, DeferredRenderer, gbuffer, grass_compute, shadow3d, overlays,
  window_geometry, event_loop, `gpu2d.shouldRenderFrame`.
- **App systems (these files):** input/players/character/ui/camera/terrain/grass/render/physics/net glue.
- **Promotable to `lib/app3d` later:** the `App` skeleton + fixed-step loop (an "engine
  app" template), the job/net seams, and TerrainSystem/GrassSystem (generic infinite
  terrain/grass). Design them so promotion is a move, not a rewrite.

## Migration (done; kept as history)

Headless parity is the guardrail:
`RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT=/tmp/walker.bmp rae run --target compiled main.rae`

1. Borrow/reference spike (done — nested `mod app.a.b` writes back on compiled).
2. Scaffold `app.rae`: `App` + `Frame`, move setup into `createApp()`, loop into
   `runWalkerApp()`; `main.rae` still calls it. Verify screenshot.
3. Extract systems one at a time, screenshot-verifying after each:
   input → players → character (roster) → camera → ui → terrain → grass → render.
4. Add `physics.rae` (fixed-step + JobSystem) and `net.rae` stubs.
5. Thin `main.rae`; fold/retire `walker_*.rae`.
6. Update this doc + `docs/ui-render-loop-performance.md` cross-links; note the
   template for other examples.
