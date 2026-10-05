# Physics for Rae — design

**Engine:** Box3D (`github.com/erincatto/box3d`, Erin Catto, MIT), in **two
implementations built side by side** until the maintainer picks one
(2026-10-05, `docs/physics-two-implementations.md`): the Rae port in
`lib/physics` (its design, rules and phases are
`docs/physics-rae-port-design.md`) and upstream's C library through generated
bindings in `lib/box3d`. Both expose the ECS surface of §5. This document is
the *application-facing* design: what the games need (§0), what Box3D brings
(§1), the engine decision (§2), how the port is laid out and built (§3–§4),
the ECS layer over it (§5, `lib/physics/ecs`), determinism (§6) and the test
scenes (§7).

**Status (2026-10-04):** the port is complete through P7 — bodies, shapes
(spheres, capsules, hulls, meshes, height fields, compounds), contacts,
continuous collision, sleep and islands, the soft-step solver, sensors, all
nine joint kinds, events, world queries and the character mover — each phase
bit-exact against Box3D's own C build. The ECS layer (§5) landed with P8.

## 0. What the driving game needs

A 3D track-and-field style downstream game (a stadium on a hill, first/third
person, Steam + mobile + WASM, multiplayer). Today it has a flat ground plane,
an analytic 2D "walkable" oval test, render-only hurdles and a bare Euler
integrator over an ECS `PhysicsBody` table — no collision. Its fixed-step
seam already exists and says "when a Jolt/Box2D backend lands, this is where
per-body integration is replaced".

| need | Box3D feature that covers it |
|---|---|
| character on the venue (track, infield, stands, hill), slide/step/ground | character mover (`b3World_CastMover` / `CollideMover` / `b3SolvePlanes` / `b3ClipVector`) — geometric, app-driven |
| hurdle contact + tip-over | dynamic body + revolute joint with limit; contact/hit events |
| thrown implements (javelin, shot, hammer, an animal) | dynamic capsule/hull/compound bodies, continuous collision for fast objects, hit events to "stick" a javelin |
| queries: what is under the foot, did it land in the sector, triggers | `b3World_CastRayClosest`, shape casts, overlap queries, sensor shapes + sensor events |
| high-jump bar, water entry, steeplechase pit | thin body on pegs (joints/contacts), sensors; floating stays in `lib/water/Buoyancy` |
| ragdoll (comedy) | capsules + spherical/revolute joints with limits, motors, springs |
| multiplayer | **cross-platform determinism** (built with `-ffp-contract=off`), deterministic across thread counts; **no rollback** |

Everything through ragdolls **and vehicles** (§5.7) is covered by one engine,
which is why the first draft's "write it in Rae" recommendation is withdrawn.

## 1. What Box3D is (verified)

- Repository created 2026-05, **v0.1.0** tagged, ~40 public commits, pushed
  the day of this reading, CI badge, ~6.3 k stars. Young: expect API churn
  between tags — the port pins a commit and moves it deliberately
  (`docs/physics-rae-port-design.md` §8).
- Core: **50 `.c` files (~1.5 MB) + 274 KB headers**, 8 public headers under
  `include/box3d/` (`box3d.h` has 424 `B3_API` functions), "no dependencies
  beyond the C runtime (and `libm`)". The repo's C++ is the sokol/imgui
  *samples* under `extern/`, not the library.
- Features: continuous collision; convex hulls, capsules, spheres, triangle
  meshes, height fields, baked compounds; multiple shapes per body; filtering
  (category/mask/group + optional custom callback); ray/shape/overlap queries;
  sensors; character mover (docs mark it **experimental**); *Soft Step*
  solver; island sleep; revolute/prismatic/distance/motor/weld/wheel/
  spherical/parallel/filter joints with limits, motors, springs, friction;
  contact/sensor/body-move/joint events, all **polled** as `pointer+count`
  arrays after `b3World_Step`; recording and replay; up to 128 worlds.
- Threading: optional. `b3WorldDef.workerCount` + `enqueueTask`/`finishTask`
  callbacks; leave them null for single-threaded. Not thread-safe from
  outside; read-only queries may be called from several threads.
- Determinism: cross-platform (fp-contract disabled), same result for 2 or 8
  workers, **no rollback** (cannot restore a prior state and re-step).
- Units: MKS, tuned for 0.1–10 m objects; `b3SetLengthUnitsPerMeter`.
- **No built-in up axis.** Gravity is any `b3Vec3` (docs/samples use Y-up,
  default `{0,-10,0}`). Rae stays **Z-up** with `gravity = (0,0,-9.81)`; no
  axis conversion anywhere. The places Y-up leaks are the height-field
  definition (rows along local z, heights along local y — a height-field body
  is rotated so its local up is world +Z, or the venue uses triangle meshes,
  which are axis-agnostic, §5.5) and the character mover of Box3D's sample,
  which the port gives an up axis (§5.2).
- The single-precision build is the one ported (`b3Pos` is `b3Vec3`; the
  port's `Pos` alias). Ids are small value structs (index, world,
  generation); events reference shapes by id. The port drops `userData`
  (§5.1).

## 2. Engine decision

| | language | 3D | wasm | determinism | cost | verdict |
|---|---|---|---|---|---|---|
| **Box3D as C behind bindings** (`lib/box3d`) | C17, MIT | yes | yes | cross-platform | bindgen gaps, a C build per target, callbacks impossible | **track B** |
| **Box3D ported to Rae** (`lib/physics`) | Rae | yes | yes (Rae's own wasm path) | the same as Box3D's C, proven bit for bit | a port of ~40 K lines, done phase by phase | **track A** |
| Jolt | C++17 | yes | yes | not cross-platform by default | C shim + libc++ | no |
| an original Rae engine | Rae | yes | yes | by construction | a solver to design | no — Box3D's design is the one wanted |

**Decision (2026-10-05): build both, then choose.** Box3D is the engine. It
is built twice: as the Rae port (track A, a performance and language test as
well as a candidate) and as upstream's C library behind generated bindings
(track B). Each track gets the same two examples, a physics playground and a
vehicle. When all four are finished, the comparison in
`docs/physics-two-implementations.md` §6 decides which one Rae uses. That
document replaces the two earlier single-track decisions: the port
(2026-09-30) and the C library (research, 2026-10-01, agreed but never
applied).

The rest of this section and §3–§4 describe track A. The port keeps Box3D's architecture, algorithms and operation order exactly, so its
results equal Box3D's C bit for bit (the oracle, §4); the reasons and the
porting rules are `docs/physics-rae-port-design.md` §1 and §3.

## 3. The port — `lib/physics/`

| package | what | Box3D source |
|---|---|---|
| `math/` | scalar, vector, quaternion, matrix, transform, AABB, plane math | `math_functions.h`, `math_internal.h`, `aabb.c` |
| `geometry/` | spheres, capsules, hulls (and their builder), meshes (BVH build and queries), height fields, compounds; each shape's mass, bounds, ray/shape casts, overlaps and mover planes | `geometry.c`, `hull.c`, `mesh.c`, `height_field.c`, `compound.c` |
| `collision/` | GJK distance, shape cast, time of impact, the manifolds of every shape pair, the mover's plane solver | `distance.c`, `manifold*.c`, `mover.c` |
| `broadPhase/` | the dynamic tree (queries, casts, rebuild), the broad phase and its pair set | `dynamic_tree.c`, `broad_phase.c` |
| `container/` | the id pool, bit sets, sorting | `id_pool.c`, `bitset.c` |
| `dynamics/` | the world: bodies, shapes, contacts, the constraint graph, islands and solver sets, the soft-step solver (scalar and wide), continuous collision, sensors, joints, events, the world queries | `physics_world.c`, `body.c`, `shape.c`, `contact*.c`, `solver*.c`, `island.c`, `joint.c` and the joint kinds |
| `character/` | the character mover of Box3D's `samples/mover.cpp` | `samples/mover.cpp` |
| `ecs/` | the ECS layer (§5) | — |

The world is a value (`PhysicsWorld`) passed `mod` to what changes it — no
globals, no registry of worlds. Box3D's handles keep their shape (`BodyId`,
`ShapeId`, `JointId` with index, world and generation). What has no Rae
counterpart is left out on purpose: user-data pointers (the ECS layer keeps
its own body→entity map, §5.1), the callbacks (§5.6), the recording/replay
and debug-draw paths.

## 4. Building track A — nothing to vendor

(Track B's build, a C library fetched at a pinned commit and linked, is
`docs/physics-two-implementations.md` §4.)

The port is ordinary Rae: `open physics/dynamics/World` and the rest, built
by the compiler like any library. There is no C dependency, no per-target
library build, no link flag. Determinism comes from the Rae C backend, which
always compiles with `-ffp-contract=off` (no fused multiply-add), the flag
Box3D's own cross-platform determinism rests on.

**The oracle** (`tools/box3d-oracle/`, `docs/physics-rae-port-design.md`
§4): Box3D's C is cloned *outside* the repository at a pinned commit, built
scalar with the same flags, and driver programs write golden traces (every
float as a C hex float); the Rae fixtures `compiler/tests/cases/94x–98x`
replay them and must match bit for bit. Moving the pin is its own task.

## 5. The ECS layer — `lib/physics/ecs/` (landed with P8)

Follows `docs/ecs-general-architecture.md` / `docs/ecs-resources.md`; product
names never appear here. Every system takes only the tables it touches
(like `prevTransformSystem`), so an app's world type is its own: it holds
the tables below as fields and the resource as a plain field.

### 5.1 Resource — `Physics` (`physics/ecs/Physics`)

```rae
type Physics {
  world: PhysicsWorld                 # the port's world, by value
  fixedStep: Float                    # 1/60
  subSteps: Int                       # 4
  accumulator: Float                  # frame time not yet stepped
  maxStepsPerFrame: Int               # 4: the accumulator's cap
  stepCount: Int
  bodyEntities: List(EntityId)        # by body index: Box3D's body user data
  shapeEntities: List(EntityId)       # by shape index: its shape user data
  contactBegan: EventQueue(PhysicsContactEvent)
  contactEnded: EventQueue(PhysicsContactEvent)
  hits: EventQueue(PhysicsHitEvent)   # approach speed over the hit threshold
  sensorBegan: EventQueue(PhysicsSensorEvent)
  sensorEnded: EventQueue(PhysicsSensorEvent)
  venueBodyId: BodyId                 # the static body the venue shapes are on
  venueShapeIds: List(ShapeId)
}
```

It is called `Physics` because the port's own world is `PhysicsWorld`. One
resource per world; a menu and a game are two. `createPhysics(def:
defaultPhysicsWorldDef(), fixedStep:, subSteps:)`; the default definition is
Z-up (`gravity = (0, 0, -9.81)`, `docs/coordinate-system.md`), one worker,
continuous collision on. `consumeFixedSteps(physics, frameTime)` adds a
frame's time and returns the fixed steps to take (the accumulator is capped
at `maxStepsPerFrame` steps, so a stall slows the game instead of
spiralling); `physicsBlend` is the fraction into the next step for render
interpolation (`lib/ecs/prevTransformSystem`). The port has no user-data
pointers, so the entity maps are the resource's: `entityOfBody` /
`entityOfShape` (noEntity for the venue or a free slot). The event queues
gather a frame's fixed steps; the app calls `advancePhysicsEvents` once per
frame after every consumer.

### 5.2 Components — `physics/ecs/PhysicsComponents`

```rae
type RigidBody {                      # a simulated body; its pose lives in Transform3D
  bodyId: BodyId
  bodyType: BodyType                  # staticBody | kinematicBody | dynamicBody
  syncedTransformStamp: Int           # the Transform3D stamp physics last synchronised
}

type Collider {                       # the shapes on the entity's body
  shapeIds: List(ShapeId)
}

type CharacterBody {                  # the geometric mover; NOT a rigid body
  mover: CharacterMover               # lib/physics/character (Box3D's samples/mover.cpp), Z-up
  throttle: Vec2                      # input: x forward, y right, -1..1
  forward: Vec3
  right: Vec3
  jump: Bool                          # input, consumed by the system
  clipVelocity: Bool
  grounded: Bool                      # output
  groundNormal: Vec3                  # output
}
```

`Transform3D` stays the single source of truth for pose. Shape options
(material, density, filter, sensor, event flags) are the port's `ShapeDef`;
contact and hit events are opt-in per shape, as in Box3D.

The character is the mover of Box3D's sample, not the step-height /
slope-limit controller this section first sketched: a capsule held up by a
"pogo" spring that steps anything the spring reaches (a 0.6 m block
included, §7) and stands on slopes. `CharacterMover` takes its up axis:
`yUp` reproduces the sample bit for bit (fixture 986), the ECS uses `zUp`.

### 5.3 Systems, in the fixed-step schedule — `physics/ecs/PhysicsSystems`

`physicsFixedStep` runs one fixed step in this order; `physicsFrame(physics,
rigidBodies, characters, transforms, frameTime)` runs the steps a frame pays
for.

1. `characterSystem` — each `CharacterBody`: the jump request, then the
   mover's `solveMove` (pogo ray, collide, solve planes, cast — up to five
   rounds); writes `Transform3D.position`, `grounded`, `groundNormal`. Runs
   before the step so the step sees the characters' new places.
2. `physicsPushSystem` — a `Transform3D` whose mod stamp differs from the
   `RigidBody`'s synced stamp was written by game code: a kinematic body is
   driven there over the fixed step (`setBodyTargetTransform`), any other
   body moved there (`setBodyTransform`). Untouched entities cost one stamp
   compare.
3. `physicsStepSystem` — `stepWorld(world, fixedStep, subSteps)`.
4. `physicsPullSystem` — the body move events: only bodies that moved are
   reported, so this writes `Transform3D` for exactly those (sleeping bodies
   cost nothing) and records the new stamp so the push does not hand it back.
5. `physicsEventSystem` — the step's contact begin/end, hit and sensor
   begin/end events become `Physics*Event`s with entity ids on the queues;
   game systems read them in schedule order. Physics never calls into the
   game.

Spawn and despawn (`physics/ecs/PhysicsSpawn`): `spawnRigidBody(physics,
rigidBodies, transforms, entityId, def: BodyDef)` creates the body, maps it,
gives the entity its `Transform3D` pose and `RigidBody`;
`addSphereCollider` / `addCapsuleCollider` / `addBoxCollider` /
`addHullCollider` add shapes (mapped, recorded in `Collider`);
`despawnRigidBody(physics, rigidBodies, colliders, entityId)` destroys the
body (its shapes, contacts and joints with it) and unmaps it **before** the
caller runs `clearEntityComponents` and `freeEntity`, so a recycled index
never owns a live body (fixture 987).

### 5.4 Queries — `physics/ecs/PhysicsQueries`

`raycastClosest(physics, origin, translation, filter) ret PhysicsHit`,
`spherecastClosest`, `overlapAabbEntities`, `overlapSphereEntities` — thin
wrappers over the port's world queries (`dynamics/WorldQueries`) answering
with entities. Read-only, callable from any system.

### 5.5 Static venue

- `addVenueMesh(physics, mesh: MeshData, transform, def)`
  (`physics/ecs/VenueMesh`) turns a render mesh (`lib/Mesh3d`) into a static
  triangle-mesh collider — welded, edges identified so bodies slide over the
  seams — on the one venue body; meshes are axis-free, so a Z-up venue needs
  no conversion;
- `addVenueBox` places a box where a gameplay edge matters;
- a height field is Y-up in its own frame (rows along local z, heights
  along local y): a height-field body is rotated so its local up is world
  +Z. The ECS layer has no helper for it yet.

### 5.6 Callbacks — not ported, and what covers their uses

Box3D's callbacks (the custom pair filter, pre-solve, friction/restitution
mixing) are not ported: Rae has no function values. **Decision (P8): nothing
new is needed for the needs of §0.**

- **Filtering:** 64 category bits and a mask per shape, the group index
  (shapes of one negative group never collide — a ragdoll's parts, a thrower
  and the implement in hand), `collideConnected = false` on joints, and
  filter joints for one specific pair cover every case §0 lists.
- **Pre-solve** is used for one-way platforms and conveyors. Conveyors are
  covered by the surface material's tangent velocity (ported); no §0 need is
  a one-way platform.
- **Mixing:** Box3D's default rule (friction √(a·b), restitution max) per
  surface material, with `userMaterialId` on hit events for the game's
  audio and effects, covers spikes on track, grass, sand and water.

If a later game needs per-material-pair friction or one-way platforms, the
answer is data on the resource the solver consults (a mixing table keyed by
user material ids; a one-way normal per shape), a library change inside the
port — not a callback and not a language feature.

### 5.7 Vehicles — what Box3D offers, and versus Jolt

Box3D models a vehicle as **real rigid bodies on wheel joints**: chassis body
+ one dynamic wheel body per wheel, each attached by a `b3WheelJointDef`:

- suspension: translation along the joint's local axis with a spring
  (`suspensionHertz`, `suspensionDampingRatio`) and optional travel limits;
- drive/brake: a spin motor (`enableSpinMotor`, `spinSpeed`, `maxSpinTorque`);
- steering: a rotation about the suspension axis with its own spring/damper,
  `targetSteeringAngle`, `maxSteeringTorque` and angle limits;
- readbacks: spin speed/torque, steering angle/torque;
- `b3BodyDef.allowFastRotation` for wheels; tire grip is the wheel shape's
  surface material (`friction`, `rollingResistance`), so slicks vs. grass is a
  material, and the ground's material contributes too.

The engine ships a **`Driving` sample** (`samples/sample_joint.cpp`, registered
under "Joints"; the docs call it "Drive"): a hull chassis (`b3MakeBoxHull(2,
0.5, 1)`, density 0.5), four wheel bodies (density 2, friction 3,
`allowFastRotation`), front wheels steer, rear wheels drive, suspension limits
+ spring, and a small "keep vehicle upright" torque. That is the whole recipe
a Rae vehicle example needs; the interactive `WheelJoint` sample exposes every
knob. Community bindings already expose it (godot-box3d maps
`Box3DWheelJoint` as "a real constraint" next to Godot's ray-cast
`VehicleBody3D`).

**Jolt is more complete for vehicles**: a dedicated `VehicleConstraint` with
`WheeledVehicleController` (engine curve, gearbox, differentials, tire
friction curves, anti-roll bars), `TrackedVehicleController` (tanks) and
`MotorcycleController`, plus a ray/cast-wheel model that does not simulate
wheels as bodies (samples: `Samples/Tests/Vehicle/{VehicleConstraintTest,
TankTest, MotorcycleTest, VehicleSixDOFTest}`). If the game needed a driving
*sim* — tuned gear ratios, drift physics, tracked vehicles — Jolt wins on
features. For an arcade/comedy car (drive, steer, jump, flip, get out) the
Box3D joint model is sufficient, and the engine-level extras Jolt adds (engine
curve, gearbox, differential) are plain Rae code over the spin motor: set
`spinSpeed`/`maxSpinTorque` per wheel from a throttle, split torque between
wheels, apply a downforce impulse. That keeps the "as much Rae as possible"
goal and avoids C++.

**Decision:** stay on Box3D; add a **vehicle example** (task §8.7) written in
Rae over the port's wheel joints (P6b), porting the `Driving` sample: `VehicleBody`
component (chassis + wheel entities + joint ids), `vehicleSystem` mapping
input to steering target and spin motor, a windowed demo on the venue mesh.
Re-evaluate Jolt only if a tracked vehicle or a tire-model sim is ever asked
for.

## 6. Determinism rules

- Fixed `fixedStep`, the accumulator in the `Physics` resource (§5.1).
- One worker. The port's parallel step (P9b) must give the same results at
  every worker count, as Box3D's does; until then the step is serial.
- Every lockstep peer runs the same Rae build: the port is Rae compiled by
  the Rae C backend with `-ffp-contract=off`, so the same source gives the
  same floats on every machine; the oracle (§4) proves it equals Box3D's
  scalar C. A SIMD path (the port's `FloatWide` over `lib/Float4`) is
  checked bit-exact against the scalar one.
- Box3D has no rollback: lockstep or server-authoritative netcode fits;
  rollback netcode does not without re-simulating from a recording.
- No wall clock or randomness inside the step; the game seeds throws.
- Fixture 988 steps two ECS worlds 600 times through the same uneven frame
  times and compares every transform bit for bit.

## 6b. Threading, SIMD and Rae concurrency — what must come first?

> Written for the bindings plan, where Box3D's C scheduler and SIMD came
> with the library. With the port, both are Rae work: the wide solver runs
> on `lib/Float4`, and the parallel step is P9b
> (`docs/parallel-stages-design.md`). The argument about Rae's concurrency
> model below still holds.

**Nothing in Rae's concurrency needs to land before Box3D.** Box3D brings its
own **internal task scheduler**: set `b3WorldDef.workerCount = N` and it
creates N-1 threads itself and uses the calling thread as the Nth; the
`enqueueTask`/`finishTask` callbacks are only for plugging in an *external*
scheduler. Its parallelism is **data parallelism inside `b3World_Step`**
(islands, contact solving), explicitly not task parallelism, and it is
deterministic regardless of worker count. So physics can be multithreaded on
day one with zero Rae threading — and single-threaded (`workerCount = 1`) is
the phase-1 default until determinism across peers is validated.

**SIMD** is likewise inside Box3D (SSE2/NEON, or `-msimd128 -msse2` on wasm,
or `BOX3D_DISABLE_SIMD`). Rae has no SIMD types of its own and does not need
them for this: Rae's `Float` = f32 was chosen with SIMD lane width in mind
(`docs/primitive-types.md`), and the Rae side of physics is control flow over
component tables, not inner solver loops. A Rae SIMD story is a separate,
later language question; it is not a physics prerequisite.

**Threading Rae's own game systems** is governed by `docs/concurrency-model.md`,
which already exists and is partly implemented — this design adds nothing to
it and depends on nothing unimplemented in it:

- Rae **inverts** `async`/`await`: normal calls are synchronous; `spawn
  f(args)` is the only marker and returns `Task(T)`; `task.get()` joins
  explicitly; dropping a task joins it (no leaks, no silent detach); there is
  **no `await`, no `async`, no function colouring** — the call site, not the
  function, decides. Race-freedom is proven at the `spawn`/`parallelLoop`
  boundary from the existing `own`/`copy`/`view`/`mod` modes instead of a
  lock subsystem. `taskScope { }` (structured concurrency) and `parallelLoop`
  exist as keywords; `Channel(T)` exists; `detach` is not yet implemented.
- What runs today on the Compiled target: `spawn` with value / `own` / `copy`
  heap args on **real OS threads** (deep-copied at the spawn site,
  ASan-clean), `get()`, join-on-drop, `taskScope`; `parallelLoop` compiles to
  a **sequential** loop for now; `mod`/`view`-aggregate captures fall back to
  sequential. The concurrency doc's §5 "current state" predates the C-backend
  work its own header describes — worth a refresh, but not a blocker.

**async/await versus game-engine multithreading — are they the same thing?**
No. `async`/`await` (JS, C#, Rust, Python) is a **concurrency** model for
*latency hiding*: many logical activities waiting on I/O interleave on few
threads, and the syntax marks suspension points. A game engine's frame is a
**parallelism** problem: a fixed budget of CPU work (physics islands,
animation, culling, ECS systems) that must be *finished* every 16 ms on as
many cores as exist, with results merged deterministically. The state of the
art there is not async/await but **job systems with fork-join / task graphs**
— Naughty Dog's fiber job system, Unity's C# Jobs + Burst over ECS, Unreal's
TaskGraph, Bevy/flecs scheduling ECS systems in parallel from their declared
read/write sets, id Tech's job lists. Their shared shape: *the frame is a DAG
of short jobs; each job declares what it reads and writes; a scheduler runs
independent jobs concurrently and joins before dependents*. Async/await only
belongs at the edges — network, disk, asset streaming — where waiting, not
computing, is the cost.

Rae's model is already the right one for a game engine, and unusually so:

1. **`spawn` + `get()` + `taskScope` is fork-join**, the core of every job
   system, without async colouring.
2. **`parallelLoop` is the data-parallel job** (one iteration per entity range
   over a dense component table), and the `own`/`view`/`mod` modes are exactly
   the "declared read/write set" a job scheduler needs — Rae has it per
   parameter *for free*, where Bevy derives it from system signatures and
   Unity from `[ReadOnly]` attributes.
3. **The ECS `Schedule`** (`docs/ecs-general-architecture.md`) is the frame
   DAG: it already records what tables each system reads; the natural
   evolution is to run systems with disjoint read/write sets concurrently
   under `taskScope`, joining at each dependency edge — the Bevy design, with
   Rae's mode system as the proof instead of runtime borrow tracking. That is
   the "best model for Rae" and it needs no new keywords.
4. Physics fits the DAG as one node: `physicsStepSystem` is a job that
   parallelises *internally* (Box3D workers) while Rae-level jobs that do not
   touch physics tables (animation, UI layout, audio) run alongside it. Later,
   when Rae-function-to-C callbacks exist, Box3D's external scheduler can be
   backed by Rae's own pool so the two do not oversubscribe cores — an
   optimisation, not a requirement.

Recommended order, therefore: **physics now** (Box3D, `workerCount` for
parallelism), Rae concurrency continues on its own roadmap (real
`parallelLoop`, `detach`, schedule-level parallel systems), and the two meet
at the external-scheduler hook when callbacks land.

## 7. Testing (TDD, non-visual)

Two kinds of fixture under `compiler/tests/cases/`:

- **Bit-exact against Box3D** (fixtures 948–986, the oracle of §4): every
  phase of the port replays golden traces of Box3D's own C — scenes stepped
  600 times with every body hash, the joints of every kind, the queries,
  and the character mover (a wall slide, a 0.3 m curb and a 0.6 m block
  stepped — the pogo spring holds the capsule's bottom 0.6 m up — a 1.0 m
  block that stops it, grounded on a 20° ramp with its normal, mesh ground
  and terrain; fixture 986).
- **The ECS layer** (P8): despawn safety — 1000 bodies spawned and
  despawned with recycled entity ids, no recycled id ever touching a live
  body (fixture 987); determinism — two ECS worlds stepped 600 times
  through the same uneven frame times, every transform identical (fixture
  988).

Still to write as the games need them (deterministic prints): restitution
(rebound height = e² × drop), friction (a box holds on 20° at μ=0.5, slides
at 0.2), projectile range v²/g at 45°, a revolute hurdle pushed at the top
rotating to its limit and resting. Plus one windowed example (a generic
"field demo": mover, a row of hinged boxes, a thrown ball) for the manual
visual gate only (P10).

## 8. Phased plan

The order of the remaining work for both tracks is
`docs/physics-two-implementations.md` §5. Track A's phases are
`docs/physics-rae-port-design.md` §7: P0–P8 and P9a have landed, P9b (the
parallel step) is next. The demos are now four examples, the playground and
the vehicle on each track.

## 9. Open questions for the maintainer

1. ~~Box3D as C behind bindings~~ — both, then decide on measurements
   (§2, `docs/physics-two-implementations.md` §6).
2. Netcode: lockstep (needs §6 across all peers, including wasm) or
   server-authoritative?
3. Building the C library: track B (`docs/physics-two-implementations.md`
   §4); its first task decides how a Rae app links it.
4. ~~Venue collision from render `MeshData` or hand-placed shapes~~ — both
   are there (§5.5): `addVenueMesh` and `addVenueBox`.
5. Is the sample's pogo mover right for the runner (it steps 0.6 m, §7), or
   should a game add its own step-height limit over it?
