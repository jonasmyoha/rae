# Physics for Rae — design (Box3D)

**Status:** proposal, revised. Supersedes the first draft, which wrongly
claimed no "Box3D" exists. It does: **`github.com/erincatto/box3d`** — Erin
Catto's 3D engine, **portable C17, MIT**, and it is the engine this design
adopts. Nothing here is implemented; §9 lists the open questions and §8 the
phased tasks (not queued until the maintainer confirms).

Facts below were read from the repository on 2026-09-13 (headers, docs,
`types.c`, `CMakeLists.txt`); where the design depends on something that must
be re-verified against the vendored copy at implementation time, it says so.

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
  between tags — pin the tag, regenerate bindings on a bump.
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
  axis conversion anywhere. The one place Y-up leaks is the height-field
  definition (`countX`/`countZ`, heights along the field's local up) — a
  height-field body is simply rotated so its local up is world +Z, or the
  venue uses triangle meshes, which are axis-agnostic (§5).
- Types that matter for binding: `b3Vec3 {float x,y,z}`; `b3Quat {b3Vec3 v;
  float s}`; `b3Transform {p,q}`; **`b3Pos` is `b3Vec3` unless
  `BOX3D_DOUBLE_PRECISION`** (then `double x,y,z`) — the single-precision build
  is used; ids are small by-value structs (`b3BodyId {int32 index1; uint16
  world0; uint16 generation}`); defs are plain structs filled by
  `b3DefaultWorldDef()` etc. (returned **by value**); events reference shapes
  by id and carry `userData`.
- Build: CMake, `BOX3D_DISABLE_SIMD`, `BOX3D_DOUBLE_PRECISION`, emscripten
  supported (`-msimd128 -msse2` or SIMD off; pthreads optional and top-level
  only). Rae will not use CMake (§4).

## 2. Engine decision

| | language | 3D | wasm | determinism | bind cost | verdict |
|---|---|---|---|---|---|---|
| **Box3D** | C17, MIT | yes | yes (SSE2-on-wasm or scalar) | cross-platform, thread-count independent | `rae bindgen` over 8 headers, like WebGPU | **adopt** |
| Box2D v3 | C11, MIT | 2D only | yes | yes | trivial | not needed — Rae targets 3D games only (maintainer decision) |
| Jolt | C++17 | yes | yes | not cross-platform by default | C shim + libc++ in an all-C toolchain | no |
| pure Rae | Rae | yes | yes | by construction | none, but a solver to write and maintain | no — Box3D covers phases 1–3 with a maintained, deterministic solver |

**Decision: Box3D as the solver, everything above the C ABI in Rae.** The
Rae side owns the ECS surface, the character controller loop (Box3D
deliberately leaves it to the application), event draining, queries, and all
gameplay physics (drag/spin on a javelin is a velocity edit per step). This is
the same split the renderer made: Rae over generated low-level bindings, C
only where it is genuine ABI (`docs/webgpu-c-surface-audit.md`).

## 3. Rae binding layer — `lib/box3d/` (generated)

Generated exactly like `lib/webgpu/` (2 104 lines for wgpu; Box3D will be of
the same order):

```
rae bindgen $B3/include/box3d/base.h $B3/include/box3d/id.h \
  $B3/include/box3d/math_functions.h $B3/include/box3d/collision.h \
  $B3/include/box3d/types.h $B3/include/box3d/box3d.h \
  --out-dir lib/box3d --module Box3d --cheader box3d/box3d.h
```

Mapping (already what bindgen does): ids/defs/results → `c_struct` mirrors by
value; `const S*`/`S*` params → `view S`/`mod S`; data pointers and callbacks →
`Ptr`; enums → `Int32` consts; `uint64_t` flags → `UInt64`; `float`/`double`
→ `Float32`/`Float64`. Ergonomic wrappers and the `unsafe {}` obligation live
in `lib/physics/`, never in the generated files (`# raefmt: off`, regenerate,
do not edit).

Generator gaps to close first (each is a small bindgen task, verified by a
golden fixture):

1. **Preprocessor conditionals.** `math_functions.h` defines `b3Pos` twice
   under `#if defined(BOX3D_DOUBLE_PRECISION)`; bindgen reads text, not the
   preprocessor. Either teach it `#if/#else/#endif` over `-D` flags, or
   generate from a preprocessed header. The former is the honest fix.
2. **Struct-by-value returns are now common** (`b3DefaultWorldDef`,
   `b3World_CastRayClosest` → `b3RayResult`, `b3MakeBoxHull` → `b3BoxHull`,
   `b3Body_GetTransform`). The WebGPU notes call this "rare, mapped directly";
   it must be ABI-tested on arm64-macOS, x86-64 and wasm32 in the first spike.
3. **Arrays inside structs** (`uint8_t padding[7]`, `b3Matrix3`) — confirm the
   `c_struct` mirror keeps layout/size; add fixed-size array fields if missing.
4. **Event arrays**: `b3ContactEvents.beginEvents` is `Ptr` + `beginCount`;
   reading `c_struct` elements from a raw pointer needs the `view`-at-index
   helper the WebGPU layer already uses (`docs/webgpu-bindings.md`, "List vs
   Buffer vs `view`").
5. **Callbacks stay `Ptr`** — a Rae function cannot yet be passed to C. Phase 1
   therefore uses no callbacks at all: `*Closest` queries instead of callback
   queries, polled events, no custom filter/friction, `workerCount = 1`.
   Optional later: an external task scheduler backed by Rae `spawn` once
   Rae-function callbacks exist.

## 4. Vendoring and building — a C dependency, not a Rae dependency

Rae's app build is one amalgamated `rae_runtime.c` plus `out.c`, compiled by
clang natively and by wasi-sdk clang for WASM, with extra `.c` files passed
on the command line. Box3D's 50 files use file-local `static` helpers, so a
unity-include into `rae_runtime.c` risks symbol collisions and would tax every
non-physics app; a hand-maintained prebuilt like `libwgpu_native` would rot.

**Recommendation:** vendor the pinned tag at `third_party/box3d/` (source,
LICENSE, the tag in a `VERSION` file) and let the compiler build it **on
demand into a cached static library**:

- detection mirrors `uses_webgpu` (the module graph contains a `box3d/…` or
  `physics/…` module ⇒ `uses_physics`);
- on first use per toolchain/target the compiler compiles `src/*.c` with
  `-std=c17 -O2 -ffp-contract=off -I third_party/box3d/include` (+
  `-msimd128 -msse2` for wasm, or `-DBOX3D_DISABLE_SIMD`) into
  `.rae/box3d/<target>-<hash>/libbox3d.a`, keyed by the sources' content hash
  and the flag set — one build, then a link line
  `-L… -lbox3d -lm`; `wasm_build.sh` does the same with the wasi clang;
- `-ffp-contract=off` is not optional: it is what Box3D's cross-platform
  determinism rests on.

This "vendored C dependency built by `rae`" mechanism is generic (`raylib`
and `monocypher` could migrate to it) and is separate from #933's `.raepack`
dependencies, which are Rae packages.

## 5. The ECS layer — `lib/physics/` (hand-written Rae)

Follows `docs/ecs-general-architecture.md` / `docs/ecs-resources.md`; product
names never appear here.

### 5.1 Resource

```rae
type PhysicsWorld {
  worldId: B3WorldId                 # the Box3D world (c_struct by value)
  fixedStep: Float                   # 1/60, the app's accumulator drives it
  substeps: Int                      # 4
  contactBegan: EventQueue(ContactEvent)
  contactEnded: EventQueue(ContactEvent)
  hits: EventQueue(HitEvent)         # approach speed over hitEventThreshold
  sensorBegan: EventQueue(SensorEvent)
  sensorEnded: EventQueue(SensorEvent)
  staticShapes: List(StaticShape)    # venue geometry owned here (mesh/height-field data + shape ids)
}
```

One resource per world; a menu and a game are two resources (Box3D allows
128 worlds). Created with `b3DefaultWorldDef()` + `gravity = (0,0,-9.81)`,
`workerCount = 1`, `enableContinuous = true`.

### 5.2 Components

```rae
type RigidBody {                      # a simulated body; position lives in Transform3D
  bodyId: B3BodyId
  motion: BodyMotion                  # staticBody | kinematicBody | dynamicBody
  gravityScale: Float
}

type Collider {                       # one shape on the entity's body; a compound is several entities or a baked compound
  shapeId: B3ShapeId
  shape: ColliderShape                # sphere | capsule | box | hull | mesh | heightField
  isSensor: Bool
  categoryBits: UInt64
  maskBits: UInt64
}

type CharacterBody {                  # the geometric mover; NOT a rigid body
  radius: Float
  halfHeight: Float
  stepHeight: Float
  slopeLimitCos: Float
  desiredMove: Vec3                   # written by the game's movement system per fixed step
  verticalVelocity: Float
  grounded: Bool
  groundNormal: Vec3
}
```

`Transform3D` stays the single source of truth for pose (as the client's
`PhysicsBody` and `lib/water/Buoyancy` already do). Render interpolation
between fixed steps uses the existing `lib/ecs/prevTransformSystem`.

### 5.3 Systems, in the fixed-step schedule

1. `physicsPushSystem` — kinematic bodies: `b3Body_SetTargetTransform` from
   `Transform3D`; dynamic bodies whose `Transform3D` was written by game code
   (a respawn) get `b3Body_SetTransform`. Change detection via the table's
   mod stamps (`componentModStamp`) so untouched bodies cost nothing.
2. `physicsStepSystem` — `b3World_Step(worldId, fixedStep, substeps)`.
3. `physicsPullSystem` — `b3World_GetBodyEvents`: only bodies that **moved**
   are reported, so this writes `Transform3D` for exactly those (sleeping
   bodies are free). `userData` on each body is the `EntityId` bits.
4. `physicsEventSystem` — drain contact/sensor/hit events into the
   `EventQueue`s (entity ids recovered from shape `userData`); game systems
   consume them in schedule order. Physics never calls into the game.
5. `characterSystem` — the Box3D-documented loop, in Rae: desired translation
   → `b3World_CastMover` → move by fraction → `b3World_CollideMover` →
   assemble `b3CollisionPlane`s (step-up and slope filtering happen here) →
   `b3SolvePlanes` → apply delta → `b3ClipVector` on the velocity; writes
   `Transform3D`, `grounded`, `groundNormal`. Runs *before* the step so
   kinematic pushes see the new pose.
6. Spawn/despawn helpers: `spawnRigidBody(world, entity, def…)` creates the
   body and shapes and sets `userData`; `despawnRigidBody` destroys the body
   **before** `clearEntityComponents`, so a recycled index never owns a live
   Box3D body. Static venue shapes are created once on a single static body.

### 5.4 Queries

`raycastClosest(world, origin, direction, maxDistance, mask) ret RayHit`,
`spherecast`, `overlapAabb`, `overlapSphere` — thin wrappers over the
`*Closest`/overlap functions returning Rae structs with `EntityId`s. Read-only,
callable from any system.

### 5.5 Static venue

- terrain / the hill: `b3HeightFieldDef` from the existing `MeshData` grid
  (`lib/Mesh3d.sampleHeight` already samples it) on a static body rotated so
  the field's local up is world +Z (verify orientation in the spike), **or**
  a triangle mesh — meshes are axis-free and `b3CreateMesh` builds the BVH;
- stands, props, track edge: `b3CreateMesh` from the render `MeshData`
  (welded, `identifyEdges` on) as static shapes; a hand-placed box where a
  gameplay edge matters;
- the client's analytic `stadiumWalkable(x, y)` retires: the mover slides
  against real geometry.

### 5.6 Runtime hooks (phase 2)

`b3SetAllocator` → the Rae runtime allocator so `RAE_MEM_STATS` accounts
Box3D memory; `b3SetAssertFcn`/`b3SetLogFcn` → the Rae log so an engine assert
is a Rae diagnostic, not a silent abort. These are C-side one-liners in the
runtime behind `RAE_HAS_PHYSICS`, the only physics C Rae will have.

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
Rae over the wheel-joint bindings, porting the `Driving` sample: `VehicleBody`
component (chassis + wheel entities + joint ids), `vehicleSystem` mapping
input to steering target and spin motor, a windowed demo on the venue mesh.
Re-evaluate Jolt only if a tracked vehicle or a tire-model sim is ever asked
for.

## 6. Determinism rules

- Fixed `fixedStep`, accumulator in the app (the client already has one).
- `workerCount = 1` in phase 1 (no callbacks available anyway); Box3D
  guarantees identical results when workers are added later.
- Build every lockstep peer with the **same** Box3D configuration: same tag,
  `-ffp-contract=off`, same SIMD choice (scalar vs SSE2/NEON vs wasm SSE2
  emulation may differ — pick one per shipped platform set and test with
  `test_determinism`-style fixtures across machines).
- Box3D has no rollback: lockstep or server-authoritative netcode fits;
  rollback netcode does not without re-simulating from a recording.
- No wall clock or randomness inside the step; the game seeds throws.

## 6b. Threading, SIMD and Rae concurrency — what must come first?

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

Golden fixtures under `compiler/tests/cases/` (deterministic prints):

- bindgen fixtures for §3 gaps: struct-by-value return round-trip, `#if`
  handling, array fields, event array reads;
- "hello box3d": drop a sphere on a static box, step 120×, print `z` — the
  first thing to make green on native and wasm;
- restitution: rebound height = e² × drop; friction: box holds on 20° at
  μ=0.5, slides at 0.2; projectile range `v²/g` at 45°;
- character: slides along a wall, steps a 0.3 m curb, is blocked by 0.6 m,
  reports `grounded` and slope normal on a 20° ramp;
- revolute hurdle: pushed at the top, rotates to its limit and rests;
- despawn safety: spawn/despawn 1000 bodies, recycled entity ids never touch
  a live body (mirrors the client's generation test);
- determinism: two worlds stepped 600× print identical transforms.

Plus one windowed example (a generic "field demo": mover, a row of hinged
boxes, a thrown ball) for the manual visual gate only.

## 8. Phased plan (proposed queue tasks; next free id is #939)

1. **Spike:** vendor Box3D v0.1.0 at `third_party/box3d/`, the on-demand
   `libbox3d.a` build + `uses_physics` link (native + wasm), a hand-written
   10-extern "hello box3d" proving struct-by-value ABI on arm64/x86-64/wasm.
2. **bindgen gaps** (#if/#else, struct returns, array fields) + generate
   `lib/box3d/` with a regeneration command in `docs/box3d-bindings.md`.
3. **`lib/physics/` core:** `PhysicsWorld`, `RigidBody`/`Collider`,
   push/step/pull systems, spawn/despawn, event draining; fixtures 7.2–7.3.
4. **Static venue + queries:** mesh/height-field colliders from `MeshData`,
   raycast/shapecast/overlap wrappers; orientation verified.
5. **Character controller** in Rae over the mover primitives; fixtures 7.4.
6. **Joints + demo:** revolute/spherical wrappers, hurdle and bar, the
   windowed example.
7. **Vehicle example:** wheel-joint wrappers, a `VehicleBody` component +
   `vehicleSystem` (input → steering target + spin motor, torque split), a
   windowed drivable car on the venue mesh, porting Box3D's `Driving`
   sample; non-visual fixture: a driven car on flat ground reaches a target
   speed and a steered car turns (deterministic prints).
8. **Runtime hooks:** allocator/assert/log into Rae's runtime accounting.
9. **Downstream migration guide** (generic wording): `PhysicsBody` →
   `RigidBody`, ground plane → static shapes, hurdles → hinged bodies,
   javelin stick = hit event → weld/static.
10. *(later)* `workerCount > 1` once cross-peer determinism is validated;
    external task scheduler via Rae's pool when Rae-function callbacks reach
    the FFI; ragdoll rig helpers.

## 9. Open questions for the maintainer

1. Confirm **Box3D** (over Jolt/pure Rae) and vendoring at the **v0.1.0 tag**
   with bindings regenerated on each bump. (Box2D for 2D: dropped — 3D only.)
2. Netcode: lockstep (needs the §6 same-configuration rule across all peers,
   including wasm) or server-authoritative?
3. The on-demand `libbox3d.a` build inside the compiler (§4) vs. a documented
   one-time `make box3d` in the toolchain checkout — the former is zero-config
   for downstream projects, the latter is less compiler code.
4. Venue collision from render `MeshData` (recommended) or hand-placed shapes?
5. Is the mover's "experimental" status acceptable for the first sport, or
   should the runner be a kinematic *rigid body* on the solver instead
   (simpler, but fights the solver on stairs and edges)?
