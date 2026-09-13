# Physics for Rae — design

**Status:** proposal, awaiting the maintainer's decision on §2 (engine choice).
Nothing here is implemented. The driving client is a 3D track-and-field style
game (a stadium on a hill, first/third-person, Steam + mobile + WASM,
multiplayer) that today has only a flat ground plane, an analytic 2D
"walkable" test for the oval, render-only hurdles, and a bare semi-implicit
Euler integrator over an ECS `PhysicsBody` table with no collision at all.
Its physics seam already exists (a fixed-step accumulator calling a system
that iterates a component table) and already says "when a Jolt/Box2D backend
lands, this is where per-body integration is replaced".

## 0. What the game actually needs

From the events list (110 m hurdles first; javelin, hammer, shot put, high
jump, steeplechase, diving, 10 k, relay; throwing an animal instead of the
implement), in order of appearance:

| need | shape of the problem | phase |
|---|---|---|
| character on ground | kinematic capsule vs a mostly-flat venue (track, infield, stands, a hill) with slide, step-up, ground normal, slopes | 1 |
| hurdle contact | capsule vs a small box; the hurdle tips over (one-axis hinge with a stop) or is knocked flat; the runner stumbles (gameplay, not physics) | 1 |
| thrown implements | ballistic rigid bodies (sphere/capsule) with gravity, optional drag and spin, landing on the field, sticking (javelin) or rolling to rest (shot) | 1 |
| ray / overlap queries | "what is under the foot", "did the javelin tip land inside the sector", trigger volumes (water pit, finish line) | 1 |
| high-jump bar | a thin dynamic bar resting on two pegs; knocked off by a capsule | 2 |
| water entry (diving, steeplechase pit) | sensor + a splash impulse; the existing `lib/water/Buoyancy` ECS system already handles floating | 2 |
| ragdoll (comedy) | articulated capsules + joints — the one item that is *real* rigid-body physics | 3 |
| multiplayer | the sim must be **deterministic** on one machine and ideally across machines for lockstep; no wall clock, no thread order in results | 1 |

Everything in phases 1–2 is "game physics": kinematic characters, a handful
of free bodies against a static world, queries, sensors, one joint type. Only
phase 3 needs a general constraint solver.

## 1. The constraints Rae imposes

- **The app is one amalgamated C translation unit** (`rae_runtime.c` includes
  every `runtime_*.c`; `copy_runtime_assets` copies them plus single-file
  vendored libs — `lodepng.c`, `stb_image.h` — into the build) recompiled with
  each app, linked with clang natively and with clang `wasm32-wasip1`
  (`-msimd128`, optional `-pthread`) for WASM. A physics dependency must fit
  that: C sources that compile in both, or a prebuilt static library per
  target like `libwgpu_native`.
- **The binding model is C-header-driven.** `rae bindgen <header.h>` emits
  low-level Rae bindings (handles → `Ptr`, enums → `Int32` consts, structs →
  `c_struct` mirrors, functions → `unsafe extern("symbol")`, callback fn
  pointers → `Ptr`). Ergonomic wrappers live in a separate module and carry
  the `unsafe {}` obligation. C++ has no path here except through a C shim.
- **The renderer rule generalises** (`docs/webgpu-c-surface-audit.md`): the
  logic is Rae over low-level bindings; C is only genuine ABI/platform. A
  physics layer must not become a pile of `rae_ext_physics_*` helpers.
- **ECS is the architecture.** Bodies are entities; state lives in components;
  singletons are resources on the World; systems run from a `Schedule`;
  `Transform3D` is the one source of truth for position (the client's
  `PhysicsBody` and `lib/water/Buoyancy` already do exactly this).
- **No collision primitives exist yet.** `lib/Vec3` / `Quat` / `Math3d` are
  complete (dot, cross, normalize, slerp, Mat4); `lib/Mesh3d` has
  `sampleHeight` for a heightfield and box/sphere/plane generators; nothing
  for AABB, ray, sphere, capsule, OBB, triangle intersection.
- Language rules: Z-up, metres, `Float` is f32, every binding typed, no
  globals, 1000-line file cap (so the library is many small modules), camelCase
  everywhere.

## 2. Engine choice — the decision to make

### Candidates

| | language | dimension | wasm | determinism | bind cost | verdict |
|---|---|---|---|---|---|---|
| **Box2D v3** (Erin Catto) | C11, MIT | **2D only** | yes (`BOX2D_DISABLE_SIMD` or wasm simd) | cross-platform deterministic since 3.1 | trivial: `rae bindgen box2d.h` | the right engine for any **2D** Rae game; wrong dimension for this one |
| **Jolt** | C++17, MIT | 3D | yes (emcc) | not cross-platform by default | needs the unofficial JoltC shim + a C++ toolchain and libc++ in the link, ~1–2 MB, exceptions/RTTI settings | best general 3D engine, but it drags C++ into an all-C toolchain for a game that needs 5 % of it |
| ODE | C, BSD/LGPL | 3D | yes | no | bindgen works | dated API, quality below Jolt, still more than we need |
| Bullet / PhysX | C++ | 3D | partial | no | shim | out for the same reason as Jolt, with less to gain |
| **pure Rae** | Rae | 3D | yes, automatically | yes, by construction | none | fits phases 1–2 exactly; phase 3 (ragdolls) is where it gets expensive |

Note: there is no "Box3D". Erin Catto's 3D work is inside Box2D's *solver
research* (soft-step, TGS), not a shipped 3D library; what people call Box3D
is usually Box2D v3 (2D) or Jolt (3D).

### Recommendation

**Write the physics in Rae, as an ECS library (`lib/physics/`), with an
engine-agnostic component/query surface so a native backend can be slotted in
later if phase 3 demands it.** Reasons, in order:

1. **The need is small and known.** A kinematic capsule against static
   geometry, a few free bodies, queries, sensors and one hinge is a few
   thousand lines of Rae; a general engine's contact graph, islands, sleeping,
   continuous collision and joint zoo are unused weight.
2. **"As much Rae as possible" is the stated goal**, and it is also the
   project's thesis: the water, grass, gbuffer and 2D-UI layers were all moved
   from C into Rae over bindings. Physics being the one big C/C++ box next to
   them would be the odd one out.
3. **Determinism for multiplayer.** A fixed-step, single-threaded, entity-
   ordered Rae sim is deterministic on one platform for free and can be made
   cross-platform deterministic by avoiding transcendental-function divergence
   in the hot path (the same discipline Box2D v3 follows). Jolt is not.
4. **WASM and mobile come for free** — no second build of a C++ library, no
   `-fno-exceptions` negotiation, no libc++ in the WASM link.
5. **Debuggability and TDD.** Every collision routine is a decidable function
   with a golden test; the client's agent guide is test-first by policy.
6. **It keeps the option.** Because the *surface* is ECS components +
   resource + queries, a `physics/backend/` module that pushes those same
   components into Jolt (via JoltC + bindgen) and pulls transforms back is a
   contained later project, not a rewrite. The client's seam comment already
   assumes this shape.

**And record the 2D answer now:** for a 2D Rae game the engine is Box2D v3 —
C, MIT, deterministic, bindgen-ready in an afternoon. That is a separate,
small task (`lib/physics2d/` = generated bindings + a thin ECS wrapper) and
must not be conflated with the 3D library above.

## 3. Architecture

Everything below is `lib/`, product-name-free, and follows the ECS docs
(`docs/ecs-general-architecture.md`, `docs/ecs-resources.md`).

### 3.1 Packages

```
lib/geometry/          pure value types + intersection maths, no ECS, no state
  Aabb.rae  Ray.rae  Sphere.rae  Capsule.rae  Obb.rae  Plane.rae
  Triangle.rae  Heightfield.rae  ClosestPoint.rae  Overlap.rae
lib/physics/           the ECS library
  RigidBody.rae        components: RigidBody, Collider (one shape each)
  PhysicsWorld.rae     the resource: gravity, fixed step, substeps, tuning
  Contact.rae          Contact/Manifold value types + ContactEvent
  broadphaseSystem/    uniform XY grid (Z-up world, mostly flat) → pairs
  narrowphaseSystem/   pair → manifold (shape × shape table)
  solverSystem/        sequential impulses: normal + friction, restitution
  integrateSystem/     semi-implicit Euler, damping, sleeping
  characterSystem/     kinematic capsule: move-and-slide, step, ground
  querySystem/         raycast / spherecast / overlap over the broadphase
  jointSystem/         phase 2: hinge with limits (hurdle, high-jump bar)
  PhysicsSchedule.rae  the fixed-step schedule an app installs
```

### 3.2 Components (data on entities)

```rae
type RigidBody {
  motion: BodyMotion          # static | kinematic | dynamic  (enum, camelCase cases)
  velocity: Vec3
  angularVelocity: Vec3
  inverseMass: Float          # 0 = infinite (static/kinematic)
  inverseInertia: Vec3        # diagonal, body space (sphere/box/capsule closed forms)
  linearDamping: Float
  angularDamping: Float
  gravityScale: Float
  restitution: Float
  friction: Float
  sleeping: Bool
}

type Collider {
  shape: ColliderShape        # sphere | box | capsule | heightfield | triangleMesh
  halfExtents: Vec3           # box; capsule uses x = radius, z = half height
  radius: Float
  offset: Vec3                # local offset from the entity's Transform3D
  isSensor: Bool              # overlap events only, no response
  layer: Int                  # collision layer bit
  mask: Int                   # which layers it collides with
  meshRef: Int                # heightfield/tri-mesh: index into PhysicsWorld.staticMeshes
}

type CharacterBody {          # kinematic capsule driven by input, not by forces
  radius: Float
  halfHeight: Float
  stepHeight: Float
  slopeLimitCos: Float
  grounded: Bool
  groundNormal: Vec3
  desiredMove: Vec3           # written by the game's movement system each step
  verticalVelocity: Float     # jump / fall
}
```

Position and orientation are **not** in `RigidBody`: they are the entity's
`Transform3D`. Render interpolation between fixed steps uses the existing
`lib/ecs/prevTransformSystem` — physics writes `Transform3D`, the renderer
draws `lerp(prev, current, alpha)`. That is exactly how the client's
accumulator loop is already shaped.

### 3.3 Resources (singletons on the World)

```rae
type PhysicsWorld {
  gravity: Vec3               # (0, 0, -9.81), Z-up
  fixedStep: Float            # 1/60
  substeps: Int               # 2–4 for thrown-object stability
  solverIterations: Int       # 6–8
  broadphase: BroadphaseGrid  # cell size ~2 m over the venue XY extent
  staticMeshes: List(StaticMeshCollider)  # heightfield + triangle soups, built once
  contacts: EventQueue(ContactEvent)      # begin/end/stay, drained by game systems
  sensorHits: EventQueue(SensorEvent)
}
```

No hidden state: an app that wants two worlds (menu + game) owns two resources.

### 3.4 Systems and the fixed-step schedule

Installed by the app into its `Schedule`, run `substeps` times per fixed step:

1. `characterSystem` — applies `desiredMove`, resolves against static + kinematic
   colliders by sphere-cast and slide, sets `grounded`/`groundNormal`.
2. `integrateSystem` — gravity × scale, damping, velocity → predicted transform
   for dynamic bodies; skips sleeping bodies.
3. `broadphaseSystem` — rebuild grid cells from AABBs (cheap: N is small),
   emit candidate pairs with layer/mask filtering, deterministic ordering by
   `(entityIndexA, entityIndexB)`.
4. `narrowphaseSystem` — per pair, the shape × shape function from
   `lib/geometry` → contact manifold (point, normal, depth; up to 4 points for
   box faces). Sensor pairs go straight to `sensorHits`.
5. `solverSystem` — sequential impulses, `solverIterations` passes, warm-
   started from last step's manifold (keyed by pair id) so stacks and resting
   contacts do not jitter; restitution with a velocity threshold; Coulomb
   friction. Position correction by Baumgarte or split impulse.
6. `jointSystem` (phase 2) — hinge constraints in the same impulse loop.
7. `sleepSystem` — bodies below a velocity threshold for N steps go to sleep;
   a contact wakes them.
8. Contact begin/end diffing against the previous step's pair set → events.

All loops are `query2`/`query3` joins over component tables, like
`buoyancySystem`. The game reads results from components and drains the event
queues in its own systems — physics never calls back into the game.

### 3.5 Queries

Free functions over the resource: `raycastClosest(world, ray, mask) ret
RayHit`, `raycastAll`, `spherecast`, `overlapSphere`, `overlapAabb`. These are
also what the character system uses internally, so they are exercised every
frame.

### 3.6 Static world

Venue geometry is built once into `PhysicsWorld.staticMeshes`:

- **heightfield** for terrain / the hill (`lib/Mesh3d.sampleHeight` already
  exists; a `Heightfield` collider wraps that grid) — capsule/sphere vs
  heightfield is a closed-form local triangle test;
- **triangle mesh with a flat BVH** for stands and props (the venue meshes are
  already `MeshData`; a builder produces the collider from them);
- **boxes** for hurdles, bars, pegs (dynamic or static bodies with box colliders).

The client's analytic `stadiumWalkable(x, y)` becomes a static collider set
(track edge as a loop of boxes, or a heightfield with a steep rim) and the
character system handles the sliding it hand-codes today.

## 4. Determinism rules (written down so they survive)

- Fixed `fixedStep`; the accumulator lives in the app, never in the library.
- Single-threaded phase 1. When `spawn` is used later, it is per broadphase
  island with results merged in entity order — parallelism must not change
  results.
- Iterate tables in index order; sort candidate pairs; never iterate a map.
- No wall clock, no random inside the sim; the game seeds any randomness.
- Cross-platform: keep `sqrt`/basic arithmetic in the solver; route any
  `sin`/`cos`/`atan2` through the game's own tables if lockstep across
  platforms is ever required (it is not for phase 1; document it, do not
  build it).

## 5. Testing (TDD, non-visual)

Golden fixtures under `compiler/tests/cases/` (deterministic prints, no
window):

- geometry: ray–triangle / ray–sphere / capsule–box closest points, AABB
  overlap, heightfield sample — value tables;
- free fall matches `z = z0 − ½ g t²` to tolerance after N steps;
- restitution: a dropped sphere's rebound height = e² × drop height;
- friction: a box on a 20° slope stays (μ = 0.5) and slides (μ = 0.2);
- projectile range: a 45° launch lands at `v²/g` on flat ground;
- character: walks into a wall and slides along it; steps a 0.3 m curb, is
  blocked by 0.6 m; reports `grounded` and the slope normal;
- hinge: a hurdle pushed at the top rotates to its stop and stays;
- determinism: two identical worlds stepped 600 times print identical state.

Plus one windowed example (a generic "field demo": a capsule walker, a box
row to knock over, a thrown ball) for the manual visual gate only.

## 6. Phased plan (proposed queue tasks, not yet queued)

1. `lib/geometry/` value types + intersection maths + golden tests.
2. `lib/physics/`: `RigidBody`/`Collider`/`PhysicsWorld`, integrate +
   broadphase + sphere/box/capsule narrowphase + solver; free-fall,
   restitution, friction, projectile tests.
3. `characterSystem` (kinematic capsule, move-and-slide, step, ground) +
   raycast/spherecast/overlap queries + heightfield & triangle-mesh static
   colliders; character tests.
4. Contact/sensor events on `EventQueue`, sleeping, warm-starting;
   determinism test.
5. `jointSystem` hinge with limits; the hurdle/bar demo; the windowed example.
6. Downstream migration guide: `PhysicsBody` → `RigidBody`, analytic
   walkability → static colliders, hurdles → hinged boxes.
7. (parallel, independent) `lib/physics2d/` — `rae bindgen` over Box2D v3 +
   an ECS wrapper, for 2D games.
8. (gate, later) Ragdoll evaluation: prototype articulated capsules on the
   Rae solver; if quality/cost is not acceptable, scope a Jolt backend behind
   the same components (JoltC + bindgen + a `uses_physics` link flag modelled
   on `uses_webgpu`).

## 7. Open questions for the maintainer

1. Confirm **pure Rae for 3D** (with Box2D v3 reserved for 2D) over Jolt-via-C-shim.
2. Cross-platform lockstep determinism: needed for multiplayer, or is
   server-authoritative / state sync the plan? It changes the maths rules in §4.
3. Ragdolls: comedy essential (phase 3 committed) or nice-to-have (gate)?
4. Where does the venue's collision come from — the render meshes (build a
   collider from `MeshData`) or hand-placed colliders? Recommendation: render
   meshes for terrain/stands, hand-placed boxes for gameplay props.
