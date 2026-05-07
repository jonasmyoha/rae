# 97_tetris3d — Design

A 3D-rendered Tetris that doubles as a stress test for several Rae features
in one place: multi-file projects with auto-imports, structured game state,
and a Bevy-flavoured component/system layout that the language can express
without macros or runtime reflection.

## Goals

1. **Playable Tetris in 3D.** Same rules as 2D Tetris, rendered with raylib's
   3D primitives. Camera floats around the well; each block is a cube.
2. **Multi-file project.** Each `.rae` file under `97_tetris3d/` carries one
   responsibility. The compiler's auto-import already picks up siblings, so
   files don't need explicit cross-imports.
3. **ECS-flavoured architecture.** Bevy-style separation of *components*
   (data), *systems* (functions over `World`), and a fixed-order *schedule*
   per frame. We do **not** build a true type-erased entity-component
   storage — that would need either macros or runtime reflection. Instead,
   each component lives in a typed `List(T)` on the `World` and "entities"
   are `Int` ids that index into those lists.
4. **Particle effects + camera flair** for line clears, locks, and rotations.
5. **Heads-up UI**: score, level, lines, next piece. Plain `drawText` for
   now — the user has flagged a UI refactor as a separate task.
6. **Reuse from `94_tetris2d`** wherever pieces lift cleanly: tetromino
   shapes, rotation table, line-clear logic, scoring.

## Non-goals (for this round)

- True dynamic ECS storage (sparse sets, archetypes, query macros).
- Audio. Raylib's audio bindings aren't there yet.
- Networking, leaderboards persisted to disk. Runtime-only highscore for now.
- Touch / gamepad input.

## File layout

```
examples/97_tetris3d/
  DESIGN.md          # this document
  main.rae           # entry: schedule, frame loop
  world.rae          # World type, init, GameState enum
  tetromino.rae      # TetrominoKind enum, shapes, colours
  input.rae          # input system (DAS, soft drop, hard drop)
  physics.rae        # gravity, lock, line clears, scoring
  render.rae         # 3D scene rendering
  particles.rae      # particle component + spawn/update systems
  hud.rae            # 2D overlay (score, level, next piece)
  camera.rae         # Camera3D setup + motion system
```

Auto-import means each file just needs `import raylib` (and `math` etc.) at
the top — sibling files come along for free.

## Architecture: components, systems, schedule

### Components (data)

Stored as fields on a single `World` struct. The "entity id" is just the
index into a parallel array, plus an `active: Bool` flag for free-slot
reuse. A real ECS would compress this, but for ~20 particles + ~200 grid
cells the simple form is faster to read than a sparse-set library.

```
type Block {
  active: Bool        # false slot = free; reuse before growing
  x: Int              # grid coords (snapped to integers on lock)
  y: Int
  kind: Int           # 1..7 colour id
  bornAt: Float       # for lock-flash effect
}

type Particle {
  active: Bool
  pos: Vector3
  vel: Vector3
  life: Float         # seconds remaining
  size: Float
  color: Color
}
```

The active piece, score, and level live on `World` directly — they're
"resources" in Bevy parlance, and there's only ever one of each.

### Systems (functions)

```
inputSystem(w: mod World)        # keyboard → piece movement / rotation
physicsSystem(w: mod World)      # gravity, lock, line clear
particleSystem(w: mod World)     # update active particles
cameraSystem(w: mod World)       # smooth camera lerp
renderSystem(w: view World)      # 3D draw pass
hudSystem(w: view World)         # 2D overlay
```

Each system is a free function. A system that needs to mutate gets `mod
World`; a read-only one gets `view World`. The "schedule" is the explicit
call order in `main.rae`'s frame loop.

### Frame schedule (in main.rae)

```
loop not windowShouldClose() {
  inputSystem(w)
  physicsSystem(w)
  particleSystem(w)
  cameraSystem(w)
  beginDrawing()
  clearBackground(...)
  beginMode3D(w.camera)
    renderSystem(w)
  endMode3D()
  hudSystem(w)
  endDrawing()
}
```

This explicit order is the closest pragmatic equivalent to Bevy's `App`
schedule. If the example grows, the function list becomes a `List(System)`
with each system tagged `Update` / `Render` / etc. — but that abstraction
is overkill at this size.

## Reuse from `94_tetris2d`

Lifted verbatim or near-verbatim:
- `TetrominoKind` enum
- Rotation table (`getShapeCell`)
- `getKindId` colour mapping
- `canMove`, `lockPiece`, `checkLines`, scoring formula
- DAS / soft-drop / lock-delay timers

Adapted:
- `getModernColor` → unchanged but exposed from `tetromino.rae`
- 2D draw routine is replaced entirely; 3D version draws cubes per cell.
- `Game` becomes `World`; the 2D-specific timer fields stay.

New for 3D:
- Camera3D and a smooth-orbit `cameraSystem`.
- Particles (line clear → burst at each cleared row's centre).
- Lock-flash: brief tint multiplier on freshly locked blocks.
- 3D well frame: wireframe rectangle drawn around the playing field.

## Particle system

`World.particles: List(Particle)` with `MAX_PARTICLES = 64`. On
construction the list is filled with `active: false` slots. Spawning finds
the first inactive slot; if none, the spawn is dropped (simpler than
growing the list mid-frame).

Every frame, `particleSystem`:
1. For each active particle: `pos += vel * dt`, `life -= dt`, fade alpha
   from `life / lifeStart`. Mark inactive when `life <= 0`.
2. Simple gravity: `vel.y -= 9.8 * dt`.

`renderSystem` draws active particles as small `drawCube`s in 3D space.

## Open questions / future work

- **Audio**: needs raylib audio bindings (`InitAudioDevice`, `LoadSound`,
  `PlaySound`). Out of scope for this round.
- **Persistent highscore**: `formatTimestamp` and `writeFile` exist; just
  needs a `highscore.json`. Easy follow-up.
- **Real ECS**: macroless query syntax is awkward in Rae. Consider once
  the language has a `for entity, &Position, &mut Velocity in world {...}`
  shape that doesn't require codegen tricks.
- **3D models**: `.glb` loading. Out of scope.
- **Refactor UI** to use a layout primitive instead of hand-placed
  `drawText` — flagged by user as a separate task.
