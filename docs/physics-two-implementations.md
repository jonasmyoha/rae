# Physics — two implementations, then a decision

**Status:** plan, 2026-10-05. Decided by the maintainer. This is the current
physics plan. It replaces the single-engine decisions in
`docs/physics-design.md` §2 ("port Box3D to Rae", 2026-09-30) and
`docs/physics-box3d-port-research.md` §9 ("use the C library instead",
2026-10-01).

## 1. The decision

Rae gets **two implementations of the same engine, Box3D**, built side by
side, each with the same two example programs. When both are finished, they
are measured against each other, and the maintainer decides which one Rae
uses.

| | track A: the Rae port | track B: the C library |
|---|---|---|
| what | Box3D ported to Rae, `lib/physics/` | upstream Box3D's own C, called through generated bindings, `lib/box3d/` |
| role | a performance and language test: how close safe Rae gets to C on a large numerical program, and what the compiler needs for it | the engine as its author ships it: upstream features and fixes by moving a pin, Box3D's own SIMD and threading |
| state (2026-10-05) | complete through P9a: every subsystem bit-exact against Box3D's C (oracle fixtures 948-996), the ECS layer, single-threaded at 1.02-1.25x Box3D's scalar C build | not started |
| next | P9b, the parallel step | build integration and bindings, then the ECS layer |

Neither track is "the real one" until §6 decides. Both are maintained to the
same standard until then: fixtures, examples on the visual gate, memory and
leak checks.

### How we got here

The port was designed on 2026-09-30. On 2026-10-01 the research
(`docs/physics-box3d-port-research.md`) recommended the C library instead,
and the maintainer agreed. That recommendation was never applied: on
2026-10-02 the port phases were moved into `QUEUE.md` unchanged, a review
of those tasks kept them, and the queue ran the port to completion. Since
the port now exists and performs well, the maintainer chose (2026-10-05) to
finish both and decide on measurements instead of discarding either.

## 2. One app-facing surface, two backends

Both tracks expose the **same ECS surface**: the resource, components,
systems, spawn/despawn helpers, events and queries of `docs/physics-design.md`
§5. Type, function and parameter names and the call order are the same; only
the package differs.

- Track A: `physics/ecs/...` (exists; it is the reference surface).
- Track B: `box3d/ecs/...`, written to match it. Where Box3D's C API cannot
  give the same shape (for example a C handle where the port has a value),
  the C layer adapts, and the difference is listed in its module comment.

The ECS fixtures of track A (987, 988 and the rest of §7 in
`docs/physics-design.md`) are run against track B too, with the same expected
output. At the same Box3D commit, built scalar with `-ffp-contract=off`, the
two must agree bit for bit, since the port is bit-exact against that build.
Track B's product build uses Box3D's SIMD path; whether its results still
match the scalar ones is measured and recorded, not assumed.

### Track B's ECS layer (B2, 2026-10-05)

`lib/box3d/ecs` has the port's modules and names:
- `Physics`, `PhysicsComponents`, `PhysicsSpawn`, `PhysicsSystems`,
  `PhysicsQueries`, `VenueMesh` and `CharacterMover`;
- `PhysicsTypes`, which holds the value types the port spreads over its
  `WorldTypes`, `QueryTypes`, `TransformMath` and `AabbMath` (`BodyDef`,
  `ShapeDef`, `WorldDef`, `BodyId`, `ShapeId`, `BodyType`, `Sphere`,
  `Capsule`, `QueryFilter`, `Aabb`, `Transform`, with the same defaults),
  and their conversions to the C structs.

An app moves between the tracks by its `open` lines:
- **Fixture 999** is 988 (two worlds, a venue mesh, a box stack, thrown
  balls, a kinematic platform, a teleport and a walking character) with only
  its `open` lines changed. It prints 988's expected output byte for byte.
- **Fixture 1000** is 987 (1000 bodies spawned and despawned over recycled
  entity ids). It prints 987's expected output; its invariant checks ask the C
  library (`b3Body_IsValid`, `b3World_GetCounters`) where 987 reads the
  port's world.

Both match with the scalar library (`RAE_BOX3D_SCALAR=1`) and the SIMD one,
which the runner uses. The ECS default worker count is the pool's, so Box3D
runs its own thread scheduler and still gives the port's bits.

What the C library forced, each listed in its module comment:
- `Physics.world` is the C world's handle. `destroyPhysics` frees it with the
  venue's meshes; the port gained the same function, which does nothing
  there.
- A `Hull` is the C library's (`createHull` from points, freed with
  `destroyHull`). Meshes and hulls are integer handles from the glue, so no
  app struct holds a raw pointer, which would force `unsafe` on its
  construction.
- The mover's internal state is in Box3D's C types (`b3Vec3`,
  `b3CollisionPlane`), and its math is Box3D's inline math in the sample's
  order. `CharacterBody` reports in Rae types as before.
- The calls that take callbacks or return arrays behind pointers go through
  `tools/box3d/rae_glue.c`:
  - the mover's collide and cast;
  - the overlap queries;
  - the closest shape cast;
  - the event arrays;
  - mesh and hull creation.

  The glue is compiled into `libbox3d.a` by `build.sh` and bound by the
  hand-written `lib/box3d/Box3dGlue.rae`. It holds no state.

## 3. The examples: two programs, each built on both tracks

| program | track A | track B |
|---|---|---|
| physics playground | `examples/122_physics_playground_port` | `examples/123_physics_playground_c` |
| vehicle | `examples/124_vehicle_port` | `examples/125_vehicle_c` |

- **The scenes are written once.** Scene descriptions (bodies, shapes,
  joints, the vehicle's chassis and wheels, the thrown ball) and the HUD are
  backend-neutral Rae in one shared package. Each example is a thin entry
  that builds those descriptions through its own track's ECS layer and feeds
  the HUD from its stats. The first example task picks where the shared
  package lives (a path dependency of both examples, or `lib/` if the
  benchmarks use it too) and records it here.
- **The same gates for all four:**
  - the visual example gate, with a headless screenshot;
  - a fixed-step determinism print: a hash of every body transform after N
    steps;
  - memory flat over a 5-minute memory test while bodies spawn and despawn;
  - `leaks --atExit` clean.

  At the same pin, built scalar, the two variants of a program print the same
  hash.
- **Featured:** both playgrounds are featured during the evaluation, side by
  side, with a README row and a screenshot each. After §6, the chosen track's
  playground stays featured.
- The playground's content is the existing brief: a large box pyramid (1k to
  10k bodies), dominoes, a hinged plank bridge and a chain, a joint-held
  stack, mesh and height-field ground, the character mover, a thrown ball, a
  performance HUD and instanced rendering. The vehicle is Box3D's Driving
  sample: a chassis and four wheel joints, steering, spin motors and a
  keep-upright torque. It also has a non-visual fixture: a driven car reaches a
  target speed, and a steered car turns.

### The shared package and the adapters (2026-10-05)

**Where it lives: `lib/physicsScenes/`.** It is a library package rather than
a path dependency because `benchmarks/physics` can use the same scenes. It
imports no physics track, only Vec3/Quat/Mesh3d and the ECS transform table:
- **`SceneTypes`:** a `PhysicsScene` (not `Scene`, which lib/ui's `ui/Scene`
  already names program-wide) is a ground (a flat box, or a Mesh3d mesh used
  as the collider too), bodies (type, one box/sphere/capsule shape, pose,
  velocity, density, friction), joints (hinge, ball or weld between body
  indices, with a frame on each body), an optional character, and a camera
  framing.
- **`SceneLibrary`:** the six scenes (pyramid sized by a body count,
  dominoes, bridge and chain, welded towers, rolling mesh, terrain walk) and
  the thrown ball.
- **`SceneHud`:** the HUD's counts, one-second step and frame timing
  windows, its text lines, and the determinism hash over every Transform3D.

**Each example has one adapter, `SceneSpawn.rae`, the only file that names
its track.** Since the track-B twin (below) everything else is shared too:
the window loop, renderer and HUD live in the package, and an example is
just `SceneSpawn.rae`, an identical `Main.rae`, and its fonts and theme. It builds a Scene through that track's ECS layer and holds the
playground's ECS world. Both ECS layers gained what the scenes need, with the
same names:
- `PhysicsJoints.addJoint` (revolute, spherical or weld);
- `physicsCounters` (bodies, awake bodies, contacts, islands, joints).

The terrain is a regular height grid used as a triangle mesh. A true
height-field shape is one of the queued items.

**Measured:** `RAE_PHYSICS_DETERMINISM=240` steps all six scenes at one
worker and at the pool's eight. Each scene's hash is identical at both counts
on track A. The same adapter with its `open` lines switched to `box3d/ecs`
prints the very same hashes on track B (scalar), so the two playgrounds run
the same simulation.

### The playground's window (2026-10-05)

`examples/122_physics_playground_port` runs on the deferred renderer:
- **Drawing** (`PlaygroundRender`): one InstanceBatch per mesh kind (a unit
  box, a unit sphere, the sphere stretched for a capsule) is refilled from
  every entity's Transform3D after the pull system and drawn with
  `drawInstances`. The ground is a one-instance batch of its mesh. The same
  bodies are queued as shadow casters grouped by kind, so each kind is one
  instanced shadow draw per cascade.
- **Limits:** the renderer's per-frame caps were 4096 draws and 4096 shadow
  casters. They are now 16384 (`gbufferMaxDraws`, `shadowMaxDraws` and the
  shadow buffer sizes), so the 10k-body pyramid draws in full: 9 870 boxes,
  9 870 casters.
- **HUD and input** (`PlaygroundUi`): a lib/ui scene shows the six
  `hudLines` and the body-count slider (app3d's Slider, 1k-10k, which
  rebuilds the pyramid when let go). Keys 1-6 switch scenes, R resets, a
  click casts the camera ray through the pointer and throws a ball
  (`throwBallAlong`), and a drag orbits (app3d's CameraRig).
- **Idle:** physics steps only while something is awake. When nothing needs
  drawing the loop parks in `Gpu2d.waitEvents` (lib/ui EventLoop's
  timeout), and `Gpu2d.shouldRenderFrame` keeps a hidden window from
  rendering. Measured: a visible 15-second run of the welded towers costs
  1.8 s of CPU, almost all before the towers sleep.
- **Measured at 10k bodies** (pool of 8, load ~10 from other work): a
  6-8 ms physics step, ~55 ms per frame. The maintainer may have been
  running a second build of 122 at the time, so treat these figures as
  skewed. Before timing anything, check that no other copy of a playground
  is running. Profiling the frame is the performance pass.
- The terrain stays a height grid used as a triangle mesh; a true
  height-field shape was optional and is not done.

After the maintainer's first try-out (2026-10-05) the controls changed:
- **Fly camera:** WASD moves and Q/E go down and up; Shift is fast and
  Control slow. A right drag looks around. This is CameraRig's free mode;
  the rig's new `lookWithRightButton` field frees the left button. Each scene
  opens where the old orbit would have put the eye, with a move speed scaled
  to the scene's size.
- **Machine gun:** holding the left button over the scene, or F, fires 15
  balls a second along the camera ray through the pointer. T toggles
  autofire.
- **Ball pool:** the balls live in `physicsScenes/BallPool`. It is
  backend-neutral, so the track-B twin reuses it. A ball is retired when
  the pool holds more than 400, when it is 20 s old, when it is
  `3 x cameraDistance + 100` from the scene centre, or when it falls below
  z = -20. The adapter's `despawnBall` removes its body, its components and
  its entity.
- **FPS meter:** ui/DebugOverlay's fps readout, top right, the one the other
  3D examples use.
- **Colours:** every body gets its own pastel. The hues are a golden-ratio
  turn apart by entity index.
- `RAE_PLAYGROUND_AUTOFIRE=1` opens with autofire on (for headless checks).
  `RAE_PLAYGROUND_DEBUG=1` also logs the live ball count.

### Track B's playground and the shared front end (2026-10-05)

`examples/123_physics_playground_c` is the twin on Box3D's C library.

- **What it contains:** its `SceneSpawn.rae` opens `box3d/ecs/*` where
  122's opens `physics/ecs/*` (Box3D's value types sit in
  `box3d/ecs/PhysicsTypes`). Its `trackName` and `assetBase` differ. Its
  `Main.rae` is byte-identical to 122's.
- **What moved into `lib/physicsScenes`,** so neither example carries a
  copy:
  - `PlaygroundRender`: the instanced batches, shadow casters, pastel
    materials, the camera ray, and `RenderKind`/`RenderShape`.
  - `PlaygroundUi`: the HUD. Its layout is now
    `scenes/PlaygroundHud.raescene`. The example passes its own fonts and
    theme directory.
  - `PlaygroundApp`: the window loop's shared steps. `Main` calls
    `beginPlaygroundFrame`, `readPlaygroundControls`,
    `updatePlaygroundCamera`, `playgroundShots`, `retirePlaygroundBalls`,
    `playgroundShouldStep`, `updatePlaygroundHud` and
    `finishPlaygroundFrame`, and itself makes only the track calls (spawn,
    step, throw, despawn, counts). The renderer, `UiWorld` and `UiSystems`
    stay locals of `Main`.
- **Threading:** the adapter passes `workerCount` into `b3WorldDef`. With no
  task callbacks, this Box3D version starts its built-in scheduler
  (`b3CreateScheduler`), so track B runs on Box3D's own threads, as the plan
  says.
- **Same simulation:** at 240 steps, all six scenes hash identically on the
  Rae port, Box3D scalar and Box3D SIMD, at 1 worker and at 8. The 120-step
  hashes are pinned in `tools/box3d-oracle/goldens/playground.golden`, and
  the example gate checks both examples against that one file. It checks
  123 at both its default (SIMD) link and a scalar link.
- **Gate linking:** `run_examples.sh` now adds Box3D's include and library
  to its gcc line when the generated C includes `box3d/box3d.h`. This is the
  rule `box3d_link_flags` in compiler/src/main.c applies for `rae run`.
- **Memory, track B:** Rae's counters cannot see Box3D's C heap. Each
  adapter therefore has `engineByteCount()`: `b3GetByteCount()` on track B,
  and 0 on the port, whose memory Rae already counts. At every cycled scene
  switch, `Main` logs it with no world alive. The gate requires every value
  to be the same.
  - 5-minute test, 30 switches: Box3D held 0 bytes between worlds every
    time.
  - Rae's peaks were identical within one buffer from the second round on
    (7.6 MB in 1 945 buffers).
  - Nothing was outstanding at exit, and `leaks --atExit` is clean. The same
    gate as 122 passes.
- **First sight of speed:** with the pyramid knocked awake by the gun, the
  HUD showed about 1 ms per step on track A and about 2 ms on track B. These
  are single readings on a loaded machine; the performance pass measures
  properly.

### The playground's featured row and gates (2026-10-05)

- **Featured:** `122_physics_playground_port.raepack` (category "Physics",
  `featured: "true"`), a README Featured row, and one screenshot in
  docs/screenshots (the bridge and chain).
- **Example gate** (`compiler/tools/run_examples.sh`, about 3 minutes):
  1. `RAE_PHYSICS_DETERMINISM=120` must print `identical true` for all six
     scenes and `determinism: 0 scenes differ`.
  2. A headless frame must not be blank.
  3. Memory must stay flat while running: `RAE_PLAYGROUND_CYCLE_SECONDS=4`
     switches scenes every 4 s, with autofire on and the runtime's test
     pointer aiming into the scene. The PEAK of `[mem:live]` buffers and
     bytes over a later 24 s round of the six scenes must match the round
     after warm-up.
  4. `leaks --atExit` over 20 s of the same kind of run must find no
     `ROOT LEAK`.
- **The 5-minute memory test:** 10 s per scene, 5 rounds, 29 scene
  switches, about 2 000 balls fired and retired. Every round after the
  first peaked at exactly 16 761 696 bytes in 9 040 buffers. At exit,
  0 Strings and 0 buffers were outstanding. `leaks --atExit` reports only
  AppKit's three NSXPCConnection root cycles, as for every windowed
  example.
- **Notes for the track-B twin (123):**
  - It can copy this gate branch.
  - `run_examples.sh` links every example with its own gcc line, which
    lacks the Box3D link flags that `rae` adds itself (`box3d_link_flags`
    in compiler/src/main.c). The twin's branch needs them added there.
  - The test pointer is in design units (1280 x 800), not pixels.

### The vehicle on track A (2026-10-05)

`examples/124_vehicle_port` drives Box3D's Driving sample on the port.

- **The car and course are shared data.** `lib/physicsScenes/VehicleTypes`
  and `VehicleLibrary` hold them, so `125_vehicle_c` reuses them.
  - **The car** is the sample's numbers, verbatim. It keeps its own Y-up
    local frame, and its bodies are placed with a quarter turn about x into
    Rae's Z-up world. Two changes from the sample, both from the brief:
    every wheel has a spin motor, with the drive torque split 70/30 between
    the rear and front axles; and forward is the chassis' local +x, the way
    the sample's W key really drives it. (The sample's own speed readout
    projects on -x.)
  - **The course** is a 65 x 65 height field of 4 m cells: a flat pad
    with two ramps, a wall and nine crates, then rolling hills.
  - Plus the course's render mesh, made from the height field's own
    triangles, and `wheelTargets`, which turns a throttle and steering
    command into each wheel's steering target, spin speed and torque.
- **The track adapter lives in the package.** It is
  `physicsScenes/port/VehicleSpawn`, so the example and fixture 1002 share
  one copy. 125 adds `physicsScenes/box3d/VehicleSpawn` with the same
  functions, and its `Main` differs from 124's in that one `open` line.
- **The window loop** is `VehicleApp` and `VehicleUi`: a chase camera, a
  four-line HUD and the FPS meter. W/S drive, A/D steer, R respawns, P
  toggles the autopilot. The deferred frame is now `PhysicsFrame`, shared
  with the playgrounds.
- **What track A gained:**
  - `dynamics/WheelJointControl`, which ports `b3WheelJoint_*`: the spin
    motor, the steering target, spin speed and steering angle.
  - In the ECS: `addWheelJoint`, `addParallelJointToVenue`,
    `addVenueHeightField`, the wheel setters, `wakeEntity` and
    `entityLinearVelocity`.
  - Track B needs the same ECS functions over the C library.
- **Fixture 1002_vehicle_drive:** on flat ground, full throttle reaches
  11.8 m/s, which is the motors' 30 rad/s on 0.4 m wheels. Full steering
  turns the car through more than 90 degrees while it stays upright.
- **Gate** (`run_examples.sh`, one branch for 124 and 125):
  - the autopilot's 300-step drive must hash the same at 1 and 8 workers,
    and equal `tools/box3d-oracle/goldens/vehicle.golden`;
  - a headless frame must not be blank;
  - memory must stay flat while the car and crates respawn every 4 s;
  - `leaks --atExit` must find no root leak.
- **5-minute memory test** (respawn every 5 s, 59 respawns):
  - live buffers stayed between 2 317 and 2 343, varying with the car's
    contacts, and bytes between 8.33 and 8.34 MB, with no growth;
  - nothing was outstanding at exit.
- **The autopilot** is a pure function of time. It drives straight over
  the ramp for 3 s (a 3 m jump), then weaves into the hills. Box3D's
  upright joint is a soft 0.5 Hz spring, so a car that lands on its side
  stays there, as in the sample; R respawns it.

### The vehicle on track B (2026-10-05)

`examples/125_vehicle_c` is 124's program on Box3D's C library. Its `Main`
differs from 124's in two code lines: `open physicsScenes/box3d/VehicleSpawn`
and its assets path.

- **Adapter:** `lib/physicsScenes/box3d/VehicleSpawn` is the port's
  adapter with its `open` lines switched to `box3d/ecs/*`. Its
  `engineByteCount` is `b3GetByteCount()`.
- **What track B gained,** with the port's names and signatures:
  - in `box3d/ecs`: `JointId` (with `toB3JointId` / `fromB3JointId`),
    `WheelJointSettings`, `addWheelJoint`, `addParallelJointToVenue`, the
    wheel setters, `wheelSpinSpeed`, `wakeEntity`, `entityLinearVelocity`
    and `addVenueHeightField`;
  - in the glue: `rae_b3CreateHeightField`, `_CreateHeightFieldShape` and
    `_DestroyHeightField`, as `uint64` handles. A height-field shape points
    at its data, as a mesh shape does, so `Physics` owns the handles and
    `destroyPhysics` frees them after the world. `tools/box3d/build.sh`
    rebuilds the library with the glue.
- **Same simulation:** fixture `1005_vehicle_drive_box3d` is 1002 on
  track B. It prints 1002's expected output byte for byte, with the scalar
  library and with the SIMD one the runner links. The example's 300-step
  autopilot hash equals `vehicle.golden` with SIMD and scalar, at 1 and
  8 workers. A 600-step drive, through the jump and into the hills, also
  hashes the same on both tracks (15823478267626950115).
- **5-minute memory test** (respawn every 5 s, 59 respawns):
  - Rae's live memory stayed at exactly 900 buffers and 6.65 MB;
  - Box3D's own heap was exactly 1 359 360 bytes at every respawn (the
    world with its course);
  - nothing was outstanding at exit.

  The 124/125 gate branch passes for both, as does 123 after the glue
  change.

### The vehicles featured, and gradient colours (2026-10-05)

After the maintainer's try-out:

- **Wheels look like wheels.** They are drawn as cylinders (the new
  `Mesh3d.makeCylinder` and a `RenderKind.wheel` batch with its own shadow
  pass); the collider stays Box3D's 0.4 m sphere. They now sit at ±1.2 m,
  just outside the 2 m chassis, instead of the sample's ±0.8 m underneath
  it, so they show.
- **The boxes on the ground fly when hit.** `dynamicBody` defaults to
  density 1000, so a 1 m crate weighed a tonne against Box3D's roughly 6 kg
  sample car, and could not be pushed. The crates and a new wall of six
  blocks (the static wall is gone) now have density 0.4. Driven into the
  wall at about 40 km/h, the car throws a block 19 m. The ramps stay static,
  because a ramp has to hold still to be jumped from.
- **Colours are gradients.** `SceneBody` and `RenderShape` carry a hue.
  `applyGradientHues(bodies, along)` spreads red to violet by position
  along a direction: every playground scene uses +x, so a pyramid or a row
  of dominoes is one smooth left-to-right rainbow. Each group of the
  vehicle's loose boxes runs along its own length. The gun's balls step
  slowly round the rainbow. This is drawing only: the playground hashes
  are unchanged.
- **The autopilot no longer rolls the car.** The old weave rolled the light
  sample car onto its side for 17 of 30 seconds.
  - Now: the 3 s straight run and jump, then a gentle weave at 60%
    throttle that turns back towards the middle beyond 50 m, and a 2 s
    turning reverse when it has been stuck for 1.5 s.
  - It keeps a small `Autopilot` state, which advances only with the
    fixed steps, so it stays deterministic.
  - Over 5 minutes it stays upright and within about 60 m of the middle.
- **Chase camera:** behind and slightly to the right, a three-quarter view.
- **New hashes.** The changed course and car give a new
  `vehicle.golden`, equal on the port, Box3D SIMD and Box3D scalar.
  Fixtures 1002 and 1005 print the same new readings.
- **Featured:** both vehicles have a pack, a README row and one
  screenshot each, taken after all of the above. The playground
  screenshots were retaken with the new colours.

## 4. Track B: building the C library

Built by B1 (2026-10-05).

- **The pin:** `tools/box3d/pin` is the single place holding the commit
  (`9f998c8`, the port's) and the URL. `tools/box3d/build.sh` and the port's
  oracle (`tools/box3d-oracle/oracle.sh`) both read it, so one edit moves both
  tracks. §6 also measures moving it to upstream HEAD.
- **The build:** `tools/box3d/build.sh` clones upstream into
  `~/.cache/rae/box3d/src` (`$RAE_BOX3D_CACHE`) and checks out the pin; no
  Box3D source is committed. It compiles `src/*.c` with `-ffp-contract=off`
  into three static libraries under `~/.cache/rae/box3d/install`
  (`$RAE_BOX3D`):
  - `lib/libbox3d.a`: native with SIMD (NEON / SSE2), the product build;
  - `lib/scalar/libbox3d.a`: native `BOX3D_DISABLE_SIMD`, the oracle's
    build;
  - `lib/wasm32/libbox3d.a`: wasm32-wasip1 through wasi-sdk, `-msimd128`.

  It also installs the headers and a `COMMIT` stamp. All three take 31-34 s
  on the M1 Max. `make setup` runs it when the library is missing.
- **Linking: the existing mechanism, no new surface.** wgpu-native is linked
  because the build sees the program use its bindings, and found through an
  environment variable (`WGPU_NATIVE`). Box3D works the same way:
  - When the generated C includes `box3d/box3d.h` (the `cheader` of
    `lib/box3d`), `rae run` / `rae watch` add `-I<install>/include
    <install>/lib/libbox3d.a`. `RAE_BOX3D` overrides the install, and
    `RAE_BOX3D_SCALAR=1` links the scalar library.
  - `compiler/tools/wasm_build.sh` links the wasm32 library the same way.
  - A program that imports `lib/box3d` while the library is missing fails
    with the command to run.
  - The browser (emcc) build does not link Box3D yet: it needs an emscripten
    build of the library, which is part of the WASM measurement in §6.
- **Size:** linking Box3D adds about 690 KB to a native release binary (the
  boxstack fixture is 917 KB against 229 KB for hello world; `__TEXT` +590 KB)
  and about 430 KB to a WASM module (811 KB against 378 KB).
- **Bindings:** `tools/gen_box3d_bindings.sh` runs `rae bindgen` over
  `base.h`, `constants.h`, `math_functions.h`, `id.h`, `collision.h`,
  `types.h` and `box3d.h`, in that order, into `lib/box3d/`:
  - `Box3dEnums.rae`;
  - `Box3dTypes.rae` and `Box3dTypes2.rae`;
  - `Box3d.rae`.

  They hold 715 functions, 111 structs, 10 enums, 23 integer constants and
  23 callback types. The output is deterministic, checked in, and a file-wide
  `# raefmt: off` region; the generator's header line exempts it from the
  naming rules, as `lib/webgpu` is exempt. The generator gained what these
  headers need, and the WebGPU bindings regenerate byte-identical apart from
  three fixes in `WebgpuTypes.rae`. Case by case:
  - *Functions*: found by `--api-macro B3_API`, not by a name prefix.
  - *Static inline math helpers* (`B3_INLINE`, `B3_FORCE_INLINE`,
    `B3_ID_INLINE`): bound like any function. The extern names the inline
    function and the generated C includes the header, so the call compiles
    to the inline body. No shim is needed.
  - *Structs by value*: work as they are. A parameter is `copy T` and a
    return is `T`, and the C call passes and returns the struct
    (`b3World_Step(worldId: copy b3WorldId, …)`,
    `b3Body_GetTransform(...) ret b3Transform`).
  - *Callbacks* (`typedef bool b3MeshQueryFcn(...)` and function-pointer
    fields such as the debug draw's): bound as `Ptr`. Passing a Rae function
    to C is not possible yet, so custom filters, pre-solve, the task-system
    hooks and debug draw cannot be driven from Rae. B2 decides whether the
    ECS layer needs any of them.
  - *Preprocessor branches*: `#if` / `#ifdef` / `#elif` over `defined(...)`
    are evaluated (`--define` adds macros), so the double-precision
    alternatives (`BOX3D_DOUBLE_PRECISION`) and the C++ operator overloads
    are left out, and `b3Pos` / `b3WorldTransform` are the aliases they are
    in a float build.
  - *Aliases* (`typedef b3Vec3 b3Pos;`): resolved to their target.
  - *Enums with implicit member values*: counted as in C.
  - *Comma-separated fields*: split.
  - *Array fields, nested unions, `static const` struct values and
    non-integer `#define`s* (`B3_PI`): skipped and listed in the output. The
    real C struct is used, so a skipped field only means Rae cannot name it:
    19 arrays (the box hull's tables, the profile counters), 4 nested unions,
    17 struct constants such as `b3Vec3_zero` and `b3_nullBodyId`, 5 enum
    members whose values are expressions (the flat-edge flags), and the float
    constants.
  - *An array of structs passed by pointer* (`const b3Vec3* points, int
    count`): bound as `Ptr` (a List's `.data`), not as a single `view`.
  - *Output above the 1000-line cap*: continued in numbered parts. Each types
    part imports the ones before it.
  - *A struct literal of a lowercase C type* (`ret b3Vec3 { ... }`) is not
    parsed as a literal. Bind it with its type on the left
    (`let v: b3Vec3 = { ... }`), which Rae prefers anyway.
- **The check:** fixture 998 builds the oracle's new `boxstack` scene through
  the bindings: a ground box and five rotated boxes, 60 steps. All 30 poses
  equal `tools/box3d-oracle/goldens/boxstack.golden` bit for bit with the SIMD
  library, and with `RAE_BOX3D_SCALAR=1`. The same program built for WASM
  gives the same bits.
- **Threading:** Box3D's own scheduler through `workerCount`, or later
  `enqueueTask` / `finishTask` over Rae's worker pool, whichever the step
  measurements favour. The task callbacks need Rae functions passed to C, so
  the worker pool is not an option yet.

## 5. Task order

The order in `QUEUE.md`, top to bottom:

1. **A: P9b, the parallel step.** It is already specified, independent of
   track B, and needed before any fair multi-worker comparison.
2. **B1: the C library build and its bindings**, plus a fixture that steps a
   world and prints poses equal to the oracle's.
3. **B2: the ECS layer `lib/box3d/ecs`**, matching track A's surface, with
   track A's ECS fixtures passing against it.
4. **The shared scene package and `122_physics_playground_port`.**
5. **`123_physics_playground_c`**, on the same scene package.
6. **`124_vehicle_port`**, plus the vehicle fixture.
7. **`125_vehicle_c`**, plus the same fixture on track B.
8. **Performance pass on both playgrounds:**
   - profile, frame budget and fixes;
   - scaling curves at 1-8 workers;
   - 120 Hz;
   - WASM.
9. **The comparison and the decision (§6)**, which ends with a question for
   the maintainer.

The examples come after both ECS layers on purpose. Writing the shared scene
package while only one backend exists would bake that backend's shape into
it.

## 6. The comparison

Measured on the four examples and `benchmarks/physics` (which gains a track-B
column), written up in this document:

- **speed:** step ms per scene at 1, 2, 4 and 8 workers, frame time in the
  playground at stress scale, and the WASM build in the browser;
- **behaviour:** the determinism hashes (identical across worker counts and
  across platforms where measurable), and equality between the tracks at the
  same pin;
- **cost to own:**
  - lines of Rae and C each track adds;
  - build time and binary size;
  - what moving the pin to upstream HEAD costs: for track B, measured by
    doing it; for track A, by the size of the upstream diff it would have to
    port;
- **what each makes possible:** callbacks (custom filtering, friction
  mixing), solver changes of our own, debugging in one language, and the
  compiler and language work the port drives.

The task ends with `[question]`: the maintainer picks the track. The other
one is then retired, kept as a benchmark only, or kept as a second backend,
and the docs and examples follow.
