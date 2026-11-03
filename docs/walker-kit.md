# The walker kit: a game's own parts

The walker app shell (lib/app3d: WalkerApp, WalkerAppRun, WalkerAppSetup)
is a whole app: the forest or island world, the walker, the camera, the
physics, the UI, the renderer. 136, 137 and 143 are it with settings only
(`runWalkerApp(settings:)`). A game that starts from it adds its own
components, systems and UI scenes without copying any kit file.

## The hooks

Rae has no function values; the kit is generic over the game's type
instead, the way lib/ui's dispatch is over an app's router type:

```rae
var game: MyGame = { ... }
ret runWalkerGame(MyGame, game: game, settings: settings)
```

and the game defines, for its own type:

| function | called | for |
|---|---|---|
| `walkerGameStart(game: mod MyGame, app: mod WalkerApp)` | once, the app built | its entities, components, UI scenes |
| `walkerGameUpdate(game: mod MyGame, app: mod WalkerApp)` | every frame, after the walker's update, before the render | its systems |
| `walkerGameEnd(game: mod MyGame, app: mod WalkerApp)` | before the shutdown | releasing what it made |

All three must exist (empty is fine). `runWalkerApp` is `runWalkerGame`
with `NoWalkerGame`, whose hooks do nothing, so the existing walkers run
exactly as before.

## A game's parts

- **Components**: in the game's own `ComponentTable`s, keyed by the walker
  world's entity ids (`app.world.allocator`): the game owns them
  (docs/ecs-systems-own-their-tables.md). The kit's tables (`app.world`,
  `app.physicsSystem`) are there to read and write too.
- **Things in the world**: anything the kit already draws, made with the
  kit's helpers: `spawnPhysicsCube` (dynamic), `spawnKinematicCube` (it
  stays where its `Transform3D` puts it: write the transform to move or turn
  it), with a color or a picture (`PhysicsCube.textureLayer`, a layer from
  `rendererAddSurfaceImagePixels`).
- **Systems**: plain functions called from `walkerGameUpdate`, over the
  game's tables and the app (`app.frame.deltaTime`; the walker:
  `controllerOf(controllers: app.world.controllers, entityId:
  rosterControlledEntityId(players: app.players))`).
- **UI**: `mountUiScene(world: app.inputSystem.world, uiSystems:
  app.inputSystem.uiSystems, path:, res: app.inputSystem.gpuUi)` in
  `walkerGameStart` adds the game's own .raescene over the walker's;
  `setNodeText` changes its text.

## Example 148

`examples/148_walker_game`: the 143 forest walker with a red box 6 m ahead
(a kinematic cube) carrying the game's own `SpinBox` component, a spin
system that turns it while the walker is within 4 m, and the game's own UI
scene saying so. `RAE_GAME_TEST=1` puts the box within reach and logs its
angle at frames 5 and 120 (a headless check).

## Not yet

- The game cannot add its own draw calls to the frame (only what the kit
  draws: cubes, the walker's characters); a render hook is the next step
  when a game needs its own meshes.
- `runWalkerGame` is kept thin (the hooks and four plain functions) because
  a qualified call such as `Vec3.create(...)` inside a generic function's
  body is mangled as a generic instantiation and does not build (queued).
