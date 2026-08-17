# 97 Tetris 3D Design

This example is a playable Tetris and a compact integration test for Rae's
production graphics stack: SDL3 input, the deferred WebGPU renderer, persistent
`Scene3d` entities, and an ECS-backed `.raescene` HUD.

## Boundaries

- `world.rae`, `tetromino.rae`, and `physics.rae` own game state and rules.
- `input.rae` maps SDL3 keyboard state into game actions.
- `particles.rae` updates a fixed-capacity particle component list.
- `render.rae` extracts game state into a persistent deferred `Scene3d`.
- `hud.rae` synchronizes game resources into persistent `.raescene` entities.
- `main.rae` owns the fixed-step simulation and deferred render-graph dispatch.

The game world remains deliberately simple: a typed `World` resource with a
row-major `List(Int)` board and a fixed `List(Particle)`. The renderer is the
actual renderer ECS. It allocates one mesh entity for each possible board cell,
four for the active piece, and one for each particle. Empty slots are hidden by
moving them outside the view, so gameplay never rebuilds meshes or grows render
component tables.

## Frame Schedule

The render loop is uncapped, while gameplay advances through a 60 Hz
accumulator:

```text
poll SDL3 events
run zero or more fixed simulation ticks
sync .raescene HUD components
extract transforms/materials into Scene3d
walk the deferred render graph
render ECS UI at the graph's UI pass
present once
```

This separation is required because the original gameplay timers count fixed
ticks; driving them once per GPU frame would make gravity depend on render
performance.

## Rendering

The world convention is right-handed Z-up. Board rows map from top to bottom on
the Z axis. Blocks use a generated box mesh and seven PBR materials. A dark,
shallow box behind the well provides visual depth and separation. Particles
reuse the same mesh/material resources.

The forward renderer is not used. It remains only a renderer reference path;
new 3D examples target deferred rendering.

## UI

`assets/ui.raescene` owns all visible text and panels. Rae code only updates
score, lines, level, next-piece text, and pause/game-over active state. The
shared MSDF font/theme fixture keeps this example small while still exercising
the same scene loader, ECS systems, and GPU2D composition as larger apps.

## Deliberate Limits

- No audio yet.
- The next-piece HUD uses a compact text identifier rather than a second mesh
  preview.
- The game world is not a generic ECS; introducing reflection or macros for a
  tiny fixed board would obscure the language example rather than improve it.
