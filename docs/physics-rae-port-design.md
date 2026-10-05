# Physics in Rae — porting Box3D to Rae

> **Questioned 2026-10-01:** `docs/physics-box3d-port-research.md` measures what Box3D's SIMD and threads are worth, how fast a direct Rae port would be, and the upstream churn, and recommends integrating the C library behind a Rae ECS layer instead of porting now.

**Status:** design, 2026-09-30. Nothing implemented. Replaces the *engine
strategy* of `docs/physics-design.md` (bind Box3D's C through generated
bindings) with **a port of Box3D to Rae**: as much Rae as possible, as little C
as possible. Everything else in that document — what the game needs (§0), the
ECS surface (§5.1–5.5), vehicles (§5.7), the determinism rules (§6), the test
scenes (§7) — still stands and is referenced here instead of repeated.

## 1. Decision

Box3D (Erin Catto, `github.com/erincatto/box3d`, MIT) stays the chosen engine.
It is **ported to Rae** instead of linked as C:

- The engine is Rae source under `lib/physics/`. **No C file ships with it.**
- The C budget is **zero** for correctness. Two exceptions may be *earned*
  later, only if a measurement demands them, each through an allowlist the
  same way the renderer's C surface is gated (`docs/webgpu-c-surface-audit.md`):
  a wide-SIMD contact-solver kernel (§6), and nothing else planned.
- The original C is used **only as a test oracle** (§4): a script fetches and
  builds it outside the repository to produce golden results. It is never
  vendored into the tree, never linked into an app.

Why port instead of bind — what changes against `docs/physics-design.md`:

| | bind the C (old plan) | port to Rae (this plan) |
|---|---|---|
| C in the product | ~50 C files, a vendored build per target (native, wasm) | none |
| bindgen gaps to close first | `#if` handling, struct-by-value returns, array fields, event arrays | none |
| callbacks (custom filter, friction, task scheduler) | impossible until Rae functions can be passed to C | ordinary Rae functions |
| debugging a physics bug | across an FFI boundary into opaque C | Rae source, Rae diagnostics |
| ECS integration | ids across the ABI, `userData` as integer bits | an `EntityId` field on the body |
| determinism | Box3D's C build flags | Rae's own build flags (§4), same rules |
| cost | small up front | ~45 k lines of C to port, and tracking upstream by hand |
| risk | bindgen/ABI surprises | performance (§6) |

## 2. What is being ported

Pinned upstream: **commit `9f998c8` (2026-09-27, "SAT samples")**, the head of
`main` when this was written; Box3D has one tag (`v0.1.0`) and changes quickly,
so the port follows a commit, not the tag. The port is a derived work: the MIT
notice is kept in `lib/physics/LICENSE-box3d.md`, and each ported file names the
C file it came from in its header comment.

Upstream inventory (lines, `src/` and `include/box3d/`, 68 862 in total):

| layer | C files | lines | port |
|---|---|---:|---|
| math | `math_functions.h/.c`, `math_internal.h`, `constants.h`, `core.h` | ~2 700 | yes, first |
| geometry and narrow phase | `aabb`, `sphere`, `capsule`, `hull` (quickhull), `distance` (GJK, shape cast), `manifold`, `convex_manifold` (SAT), `triangle_manifold`, `mesh`, `mesh_contact`, `height_field`, `compound` | ~18 000 | yes |
| broad phase | `dynamic_tree`, `broad_phase`, `bitset`, `id_pool`, `table` | ~3 800 | yes |
| dynamics | `body`, `shape`, `contact`, `contact_solver`, `solver`, `solver_set`, `island`, `constraint_graph`, `sensor`, `physics_world`, `types` | ~17 500 | yes |
| joints | `joint` + distance, motor, parallel, prismatic, revolute, spherical, weld, wheel | ~6 500 | yes |
| character mover | `mover.c` (70) + the application loop in `samples/mover.cpp` | ~600 | yes, in Rae as the application loop |
| C infrastructure | `arena_allocator`, `block_allocator`, `container.h`, `verstable.h` (hash table), `qsort.h`, `algorithm.h`, `ctz.h`, `rapidhash.h`, `name_cache`, `timer`, `platform.h` | ~4 000 | **replaced** by Rae (§3), except `qsort.h`, ported for identical ordering |
| threading and SIMD | `scheduler`, `parallel_for`, `simd.h/.c` | ~1 900 | scalar path only (§6) |
| recording, replay, snapshots | `recording*`, `world_snapshot` | ~7 000 | **not ported** (no current need; revisit if netcode wants replay) |

So roughly **45 000 lines of C** are ported. Rae spells named arguments and
one parameter per line past three, so the Rae side will be about the same size
or somewhat larger, split into files under the 1 000-line cap (≈ 60–80 files).
Upstream also has 18 000 lines of C tests (`test/`) and a benchmark suite
(`benchmark/`); both are inputs to §4 and §6, not ported as such.

## 3. Porting rules (C → Rae)

The rules below are what every porting task follows, so the result reads as
one codebase.

- **Types.** `float` → `Float` (f32, the same width — this is what makes bit
  equality with the C possible). `int32_t` stored in structs and arrays →
  `Int32`, loop indices and counts → `Int`. `uint64_t` masks → `UInt64`,
  `uint16_t` generations → `UInt16`, `bool` → `Bool`. Box3D's `b3Vec3`,
  `b3Quat`, `b3Transform`, `b3Matrix3` become physics-local types (`Vec3` is
  `lib/Vec3`'s layout already: three `Float`s).
- **Pointers are already indices.** Box3D stores bodies, shapes, contacts and
  joints in arrays addressed by `id = index + generation`; its pointers are
  short-lived views into those arrays. In Rae they are `view`/`mod` bindings
  (`let body: mod Body => world.bodies.modAt(index: i)`), never stored.
- **Unions** (a shape is a sphere *or* a capsule *or* a hull …) become an enum
  tag plus the per-kind data. Rae has no sum types by design
  (`docs/match-and-sum-types.md`); the per-kind data either sits in the shape
  record or in a per-kind table indexed from it, chosen per case for layout.
- **Function-pointer tables** (manifold functions per shape pair, per-type
  joint dispatch, callbacks) become exhaustive `match` on enums: adding a
  shape or joint kind then fails to compile at every site that must handle it.
  User-supplied callbacks (custom filter, friction/restitution mixing) become
  plain Rae function parameters of the step, not stored pointers.
- **Allocators and containers.** `b3Array`, the arena and block allocators and
  the id pools become `List(T)` / `Buffer` owned by the world; the pair hash
  set (`verstable.h`) becomes a Rae open-addressing map keyed by the `UInt64`
  pair key. `qsort.h` is ported as written, because sort order is part of the
  simulation result and must match the C (§4).
- **No globals.** Box3D's world registry, length-units-per-meter and
  allocator/assert hooks become fields of the `PhysicsWorld` resource and
  parameters; `lib/` is globals-clean by rule (`AGENTS.md`).
- **Math.** Box3D's hand-written, deterministic `b3Atan2` and
  `b3ComputeCosSin` are ported verbatim; nothing in the step may call the C
  library's `sin`/`cos`/`atan2`. `sqrt` stays `sqrtf` (IEEE exact).
- **Bits.** Word operators (`bitand`, `shl`, … — `docs/bitwise-operators.md`);
  `ctz` is `trailingZeros(x: UInt64)` (lib/Math.rae, P0): the existing
  `trailingZeros(x: Int)` family gained `UInt64`, `UInt32` and `Int32`
  overloads, and the runtime defines all the bit intrinsics `static inline`
  in `rae_runtime.h`, so each call is one CPU instruction. 0 answers the bit
  width (64 or 32); Box3D never asks for 0.
- **Asserts and validation.** `B3_ASSERT` → a Rae assert that logs the source
  line; `B3_VALIDATE` consistency checks → functions called from tests and
  debug builds only.
- **Names.** Rae style, no `b3` prefix, no abbreviations
  (`b3Body_GetTransform(bodyId)` → `bodyTransform(world:, bodyId:)`,
  `b3World_CastRayClosest` → `castRayClosest(world:, …)`). A table of the
  public mapping lives in `lib/physics/README` so the upstream docs and samples
  stay usable.
- **Order of floating-point operations is preserved** — no algebraic
  "cleanups" while porting, no reordered sums. That is what §4 checks.

## 4. Determinism and the oracle

**Build flag first (done in P0).** Clang on arm64 contracts `a * b + c`
into a fused multiply-add by default, so the same Rae program gave different
float results on Apple silicon and x86. Box3D's cross-platform determinism
rests on turning that off. Every Rae compile line now passes
`-ffp-contract=off` (`rae run`/`build`, the emcc build, `rae init`'s
Makefile, `run_examples.sh`, `wasm_build.sh`, the devtools recipes, the iOS
project, the benchmarks), and `rae_runtime.h` carries
`#pragma STDC FP_CONTRACT OFF` as the backstop for any build that does not
(every Rae translation unit includes it first). Fixture
`942_no_fma_contraction` fails if a fused result appears: built the old way
it prints `false` for both the Float and the Float64 multiply-add.

**Bit-exact differential testing.** With the flag off and the scalar path,
the Rae port performs the same float operations in the same order as the C
scalar build, so it should produce **the same bits**, not just close values.
That turns "is the port right?" into a mechanical check:

- `tools/box3d-oracle/oracle.sh` — clones Box3D at the pinned commit into
  a cache directory outside the repository (`$RAE_BOX3D_CACHE`, default
  `~/.cache/rae/box3d-oracle`), builds the library scalar
  (`BOX3D_DISABLE_SIMD`, `-ffp-contract=off`, validation off), compiles each
  driver in `tools/box3d-oracle/drivers/` against it and writes
  `tools/box3d-oracle/goldens/<driver>.golden`: one line per call,
  `<function> <inputs> -> <outputs>`, every float a C hex float (`%a`, exact;
  `strtof` reads it back to the same bits). A driver that steps a world
  creates it with `workerCount = 1`. The first golden (P0) is `math`: 2 483
  calls over 49 functions — the deterministic `b3Atan2` / `b3ComputeCosSin`
  over an edge grid and random angles, and the vector, quaternion,
  transform, 3x3 matrix and segment functions over inputs that are multiples
  of 1/256 (exact in binary32), reproduced byte for byte on a second run.
  Later goldens: per module (GJK distances, manifolds, hull construction,
  tree queries) and per scene (body transforms every step).
- The goldens are committed as text. The test suite never builds C: a Rae
  fixture runs the same inputs and compares hex bits. The oracle only runs
  when a porting task adds a module or the pinned commit moves.
- Scenes come from upstream's own tests and samples: a falling sphere on a
  box, restitution, friction on a slope, a pyramid stack, each joint type,
  continuous collision, a character against walls and steps, a mesh and a
  height field.

When a bit differs, the first differing value in the trace names the module
and step, which is where the port diverged. A deliberate divergence (none is
planned) would need an explicit tolerance and a written reason.

The rules of `docs/physics-design.md` §6 then apply unchanged: fixed step,
single-threaded first, the same build everywhere, no randomness or wall clock
in the step, no rollback.

## 5. Architecture of `lib/physics/`

```
lib/physics/
  LICENSE-box3d.md   README.md (public API mapping to Box3D names)
  math/        Vec3/Quat/Transform/Matrix3 ops, cos/sin/atan2, Aabb
  geometry/    Sphere, Capsule, Hull (quickhull), Mesh, HeightField, Compound
  collision/   Distance (GJK), ShapeCast, Manifold, ConvexManifold (SAT),
               TriangleManifold, MeshContact
  broadPhase/  DynamicTree, BroadPhase, pair set
  dynamics/    Body, Shape, Contact, ContactSolver, Solver, SolverSet,
               Island, ConstraintGraph, Sensor, World (step, events)
  joints/      Joint + one module per joint kind
  queries/     ray / shape casts, overlaps
  mover/       the character mover primitives + the application loop
  ecs/         PhysicsWorld resource, components, systems (physics-design §5)
```

Module files are PascalCase, folders camelCase (`AGENTS.md`). The engine
layers know nothing about the ECS; `ecs/` is the only part that touches
`Transform3D` and entity tables, exactly as `docs/physics-design.md` §5
describes, with `b3*` calls replaced by the Rae functions of the layers
below. A body carries an `EntityId` field directly; events carry entity ids.

## 6. Performance

The port must stay close to the C scalar build. The benchmark scenes of
upstream's `benchmark/` (and the large pyramid and joint grid) are ported as
Rae programs that print a step time; target: **the Rae port single-threaded
within 1.5× of the C scalar build** at `-O2`, measured on the maintainer's
machine and recorded in the doc.

Known risks, each addressed when measured, not before:

- **Checked element access in hot loops.** `List.viewAt/modAt` return an
  optional; the solver's inner loops iterate thousands of constraints per
  step. If the check shows up in profiles, the fix is a Rae-level unchecked
  indexed view for hot loops (a language/runtime task), not C.
  **Measured (P0, 2026-10-02): no unchecked view is needed.**
  - `benchmarks/list_access` (2026-10-01, small plain-data elements, the
    scatter kernel): `if let` + `modAt` 1.20x checked C; `copyAtDefault` +
    `set` 1.16x checked C since compiler 0.1.99 (3.9x before).
  - `benchmarks/solver_struct` (solver-sized structs: 100 000 contact
    constraints of 80 bytes, each reading and writing two 32-byte body states
    at scattered indices, 20 passes, `modAt` for the constraint and
    `copyAtDefault` / `set` for the bodies): Rae **0.98-1.02x plain C** and
    **0.98-1.09x bounds-checked C** (best of five, two runs, M1 Max at load
    ~8 from unrelated work), ~20 ms for 2 million constraint solves in each.
    The checksums match bit for bit across Rae, C and checked C, with
    `-ffp-contract=off` on all three. On structs this size the scattered
    memory traffic dominates and the bounds check disappears in it.
- **Struct copies.** Box3D passes small structs by value everywhere; the port
  uses `view` parameters for anything larger than a vector.
- **SIMD.** Box3D's contact solver runs 4/8-wide on SSE2/NEON. The port
  follows the scalar path, where a "wide float" is a struct of four `Float`s;
  clang may auto-vectorize some of it. If the scalar solver is the bottleneck
  after the other fixes, the choice is between Rae vector types (a language
  design question) and **one** C kernel on the allowlist — decided then, with
  numbers.
- **Threads.** Box3D parallelizes inside the step (graph-coloured constraint
  batches). The port is single-threaded first; parallelism comes through
  `parallelLoop` once it runs on real threads (`docs/concurrency-model.md`),
  with results unchanged because the colouring, not the scheduling, fixes the
  order. What the engine still lacks for that, measured against Box3D at 1-8
  workers, is `docs/physics-performance-plan.md`: threads are needed (no
  single-threaded build, C included, meets the featured example's 4 ms on a
  5 000-box pyramid), `Float4` is the margin, and P4 ports the step structure
  parallel-ready (§8 there).

### 6.1 Measured (P9a, 2026-10-05)

`sh benchmarks/physics/run.sh`: one worker, 60 Hz with 4 sub-steps, the C
app's step counts, best of three, ms per step; M1 Max at load ~6 from
unrelated work (`BENCH_ALLOW_LOAD=1`), Rae at the release profile with
`Float4`'s NEON lowering. Fixtures 948-988 stay bit-exact; the checksum is
the benchmark's own (FNV-1a over the final positions) and did not move
through the fixes.

| scene | C scalar | C SIMD | Rae | Rae / C scalar | Rae / C SIMD |
|---|---|---|---|---|---|
| large_pyramid | 11.31 | 7.91 | 11.12 | 0.98x | 1.41x |
| many_pyramids | 21.67 | 14.74 | 23.06 | 1.06x | 1.56x |
| joint_grid | 11.55 | 11.52 | 13.04 | 1.13x | 1.13x |
| convex_pile | 13.71 | 11.48 | 16.48 | 1.20x | 1.44x |

(P9a-8, 2026-10-05, load ~3: the ratios move with the machine's load.
Before P9a-2/P9a-5 the joint grid was 30.12 ms (2.58x), before P9a-3 the
convex pile 34.4 ms (2.50x). Both sides leave the first step untimed and
divide by the steps - 1 they time, as upstream's app does; the P9a run
timed the first step on the Rae side and divided both by all steps.)

The target (1.2x C scalar) is met by every scene since P9a-8; the convex
pile is on the line (1.20x). The large pyramid
started at **2 267 ms per step** (190x): per-element reads copied whole
structs, and the fixes were all of that kind:

- **Whole solver-set copies** (`world.solverSets.copyAtDefault(index:)`
  copies every list in the set) per contact in collide and per body in
  finalize: view accessors in `World` (`setBodySimAt`, `setBodyCount`, ...).
  2 267 → 27 ms.
- **Contact deep copies in the wide prepare** (a `Contact` owns heap lists;
  each lane copied it four times): `PrepareLane` holds only the scalars the
  prepare reads, gathered through views; `Manifold.points` is an inline
  `Array(ManifoldPoint, cap: 4)` instead of a heap `List` (Box3D's fixed
  array). 27 → 18 ms.
- **Wide constraint copies** in warm start and store impulses, the span
  list in prepare, the shapes in collide (only read when the contact is not
  recycled), the `BodySim` in integrate velocities and the `JointSim` in the
  joint threshold check: views. 18 → 14.8 ms.
- Two compiler fixes found on the way: a generic `Array` call whose result's
  field is an argument re-inferred `T` from that argument's type (fixture
  993), and a void method call on a list (`dots.add(value: x)`) was wrapped in
  a String-pool mark/flush pair because another function named `add` returns
  a value — now judged by the receiver's type (822 → 422 pool marks in the
  benchmark, convex pile 37.7 → 34.4 ms).

Where the rest goes, profiled against the C app:

- **Contacts (pyramids).** Rae's SIMD solve and push are already faster than
  C scalar's; collide and prepare are ~2.5x C's. Box3D iterates the colour's
  contiguous `b3ContactSim` array; the port reaches each contact through its
  id in `world.contacts` (432-byte `Contact`, its manifold in a separate heap
  list) and copies both `BodySim`s (224 bytes each) per contact in collide.
- **Joints (joint grid), P9a-2.** `JointSim` was 1 816 bytes: Rae has no
  union, so it carried every joint kind's record side by side where Box3D's
  union is a few hundred bytes, and the solve streamed ~4x the memory per
  joint. Now the records live in the world's `JointKindData`, one list per
  kind with a free-slot stack, and a `JointSim` (208 bytes) holds its
  record's `dataIndex` (so does the `Joint`, which frees the slot on
  destroy). The record stays put while the sim moves between sets and
  colours; the kind's prepare, warm start and solve take it as a `mod` /
  `view` parameter, as Box3D passes `&base->sphericalJoint`, instead of
  copying it in and back. Joint grid 30.1 → 15.5 ms (2.58x → 1.33x),
  fixture 984 and the checksums unchanged. The rest of the gap is not
  layout: the spherical solve compiles to Box3D's instruction count (1 140
  instructions, the same loads, stores and float operations), but the whole
  run retires 19% more instructions at a lower IPC (3.7 against 4.6): the
  per-joint bounds checks, the kind dispatch, and `jointBodyState` copying
  each `BodyState` where Box3D points at it.
- **Joint solve overhead, P9a-5.** Measured by patching the generated C in
  /tmp and counting instructions and cycles (`/usr/bin/time -l`, the setup
  subtracted, configurations interleaved with the C app and the minimum
  taken; wall time alone is too noisy at load). What paid:
  `pointToPointMass` written out in the spherical solve as Box3D writes it
  (a call clang would not inline in the eight-kind dispatch; ~5M of 193M
  instructions per step, up to 14% of the time when every site was
  inlined); the joint-event threshold pass skipped when no joint of the
  block has a threshold (Box3D checks inline after each solve; 1.4M); and
  prepare in place, the bodies read first and the sim prepared where it
  lives instead of copied out and set back (1.4M). What did not: an
  unchecked `BodyState` read in `jointBodyState` (2M fewer instructions,
  no fewer cycles), and keeping each kind's functions out of line. Joint
  grid 15.5 → 13.4 ms, 1.16x.
- **Narrow phase and allocations (convex pile), P9a-3.** Box3D keeps hulls
  in a database that shares equal hulls between shapes; the port copied the
  hull into every shape, so a pile of one convex streamed ~15 MB of copies
  where C reads one hull from cache. `addSharedHull` (WorldShapes, by
  content hash and a full comparison, HullContent) shares them: 31 → 23 ms.
  The hull narrow phase's per-pair lists (SAT's face dots, edge candidates
  and edge-pair lists, the clip polygons, the face contact's points) live
  in a per-task `HullScratch`, cleared and reused, and the clip polygons
  swap by moving instead of copying both lists per clip plane.
  The step is now allocation-free when nothing changes: the world keeps
  its `StepContext` and `PairContext` and lends them with `own` moves
  (`solve(world: mod, context: mod)` cannot take a world-owned context
  directly; moving it out for the step and back needs no language
  feature), the trees keep their rebuild, copy and leaf-walk stacks, and
  the rebuild's node arrays swap by moving (it deep-copied both arrays on
  every rebuild). Whole-struct copies that only wanted one field (TaskContext,
  GraphColor, PhysicsShape, Contact in the island and graph bookkeeping)
  read through views or move the field out and back. Allocations in a
  steady large-pyramid step (an allocation-counting runtime, full run less
  a two-step run): 431 → 0. Convex pile 34.4 → 21.2 ms (1.54x). What is
  left there is arithmetic: Box3D's SAT runs on structure-of-arrays copies
  of each hull's points and normals four lanes at a time (b3FloatW, which
  clang vectorises even in the scalar build) in the support search, the
  face separation and the edge-pair loop; the port's hull has no SoA copies
  and its loops are scalar over AoS points (findHullSupportVertex 11%,
  testEdgePairs 10% of the convex pile). Contact and island creation still
  allocate (a contact's manifold list, island merges), as the pile settles.
- **SoA hulls, P9a-6.** A Hull now carries Box3D's structure-of-arrays
  copies (`soaPointsX/Y/Z`, padded with point 0, and `soaNormalsX/Y/Z`,
  padded with zeros; `buildHullSoa`, called by every hull constructor,
  compared by HullContent). The support search, `supportWide` (Box3D's
  b3GetSupportWide with the index embedded in the mantissa) and the face
  dots run four lanes at a time on lib/Float4 in Box3D's operation order and
  tie-breaking; the generated loop is plain NEON (three loads, three
  multiplies, two adds, a compare and two selects per four vertices). The
  edge-pair phase reads one `EdgeOfA` record per candidate edge, and collide
  reads the two bodies as a 72-byte `ContactBodyPose` instead of two 224-byte
  BodySim copies. Convex pile 21.2 → 19.7 ms (1.43x). The support search now
  costs what C's does; what is left is spread evenly: every part of the
  narrow phase is ~1.4x C's (the narrow phase is ~68% of the step on both
  sides), with 25% more instructions at a lower IPC (`/usr/bin/time -l`:
  77.8G against 62.0G instructions per run, IPC 2.67 against 2.89). That
  is per-access overhead rather than one loop: bounds-checked reads of
  `List` elements where C indexes raw arrays, Int (8-byte) hull indices
  where Box3D's half-edges are four bytes, and struct copies of edges and
  planes in edgeCandidates and the face loops.
- **Memory latency, P9a-8.** Patching the generated C showed the gap was
  stalls, not instructions: removing every List bounds check cut the
  convex pile's instructions from 152M to 128M per step (C: 124M) but its
  cycles only 1%; 8-bit hull indices, a 32-bit Contact or TreeNode, -O3 or
  -mcpu changed nothing. Per stage (the C app's `-s` step profile against
  timers patched into the Rae step): collide 12.4 against 8.9 ms, pairs
  2.6 against 1.6, solve 3.7 against 3.1. Box3D prefetches the contact four
  iterations ahead in b3CollideTask (its comment: "the random 216 B contact
  load"); the port did not, and its contacts are 432 bytes. `List.prefetch`
  (lib/core; `rae_ext_rae_buf_prefetch` prefetches every cache line of the
  element, a hint that changes no result, fixture 994) lets Rae ask for the
  same: collideTask prefetches four contacts ahead (convex 19.4 → 16.8 ms,
  cycles 61 → 53M per step), the wide prepare prefetches the next wide
  constraint's four contacts (~2%), and the candidate flush prefetches the
  pair-set slots and shapes as Box3D's does (neutral here). Prefetching a
  contact's manifold block or the body sims gained nothing. The compiler
  also stopped wrapping calls with a `sizeof(T)` argument (List.create,
  grow, prefetch) in a String-pool mark/flush pair. The pyramids gained as
  much: large 13.0 → 11.1 ms, many 27.7 → 23.1 ms.
- **Contact and island churn, P9a-7.** A moving pile allocated wherever
  contacts or islands changed. Measured with an allocation-counting runtime
  (each buffer allocation counted by its callers, a full run less a shorter
  one), the convex pile's steps 460-500 made ~38 500 buffer allocations per
  step. The fixes:
  - `destroyContact` read the contact by copying it, manifold list and all
    (~6 900 per step); it reads it through a view now.
  - Continuous collision built a hits list, a fractions list and a tree
    query stack for every fast body (~29 000); they are world lists lent with
    `own`, and `beginTreeQueryWith` takes a caller's stack.
  - collideHulls' edge path built a one-point manifold per try (~800); it
    lives in HullScratch.
  - The new pair keys are recorded in `world.createdPairKeys` instead of a
    returned list.
  - A new contact reuses its slot's manifold and triangle-cache buffers
    (`zeroContactWithLists`).
  - mergeIslands, destroyIsland and trySleepIsland move an island's lists
    out instead of copying the island; island sleep's id list is a world list;
    begin/end-touch handling reads contacts through views.
  Steps 460-500 then allocated 4 per step: a ShapeProxy's point list per
  time-of-impact query (Box3D's proxy is a fixed array) and QuickSort's range
  stack. P9a-9 removed both: continuous collision lends two world point
  lists to the time-of-impact proxies (`shapeProxyPoints` fills them, the
  query moves them back), and the pair keys sort with a world-owned range
  stack (`quickSortWithStack`). Steps 460-500 now allocate 2 buffers in 40
  steps, both the capacity of collide's world-owned contact list growing
  with the contact count: once a pile stops creating contacts its step
  allocates nothing. While the pile settles (steps 2-40), 57 of its 62 allocations per
  step are new contact slots: the contact count grows to ~56 000, and a slot
  gets its manifold list once. A steady large-pyramid step still allocates
  nothing. The move needed a compiler fix: `own` of a `mod` parameter
  emitted the pointer (fixture 995).
  (A correction to P9a-3's method: a stray second `Main.rae` in the
  directory of the shorter run broke its C build from P9a-3 on, so the
  profiler compared against a stale binary. Re-measured with the build
  checked, the large pyramid's steady step is still 0.)

These are queued as P9 follow-ups.

## 7. Phases (queue tasks)

Each phase lands green: the full suite, its oracle fixtures bit-exact, and no
C added.

- **P0 — prerequisites (done 2026-10-02).** `-ffp-contract=off` in every
  Rae build with a fixture; `trailingZeros(x: UInt64)` (and `UInt32`,
  `Int32`) as an inline intrinsic; the oracle tooling (`tools/box3d-oracle/`)
  with its first golden (the math module); the measurement of checked access
  on solver-sized structs (§6: no unchecked view needed).
- **P1 — math (done 2026-10-03).** The math layer, bit-exact against the
  oracle: `lib/physics/math/` (scalars, vectors, quaternions, matrices and
  inertia, transforms, AABBs, segments, planes, 2D) and `lib/physics/Constants.rae`;
  the golden grew to every math function (154, 4 944 calls) and fixture 948
  matches all of them bit for bit. Two compiler/stdlib pieces came with it: a
  float literal in a `Float` (f32) expression is now emitted as an f32 literal
  (`0.5f`), so `0.5 * x` is single-precision arithmetic as in C, not a double
  round trip; and `remainder(x:y:)` in lib/Math (libm `remainderf`, what
  `b3UnwindAngle` calls). Name mapping: `lib/physics/README.md`.
- **P2 — convex geometry and narrow phase.** AABB, sphere, capsule, hull
  (quickhull), GJK distance and shape cast, manifolds for convex pairs.
  **Landed 2026-10-03, bit-exact:** the AABB ray cast, spheres, capsules,
  GJK distance (warm-started), the shape cast, sweeps and time of impact
  (`lib/physics/geometry/`, `collision/Simplex|Distance|ShapeCast|TimeOfImpact`,
  golden `geometry`, fixture 949); feature pairs, polygon and segment
  clipping, the four-point reduction and the sphere/capsule pair manifolds
  (`collision/Manifold|ConvexManifold`, golden `manifold`, fixture 950).
  The hull (`hull.c`: quickhull, generated and box hulls, cloning, mass,
  queries, the 2D hull; `geometry/Hull*|BoxHull|Hull2d`, golden `hull`,
  fixture 951) landed 2026-10-03, bit-exact, quickhull index-linked.
  The hull manifolds (hull-sphere, hull-capsule, hull-hull with the scalar
  SAT and its cache) and the shape-pair dispatch as an exhaustive `match`
  (`collision/SeparatingAxis|HullManifold|HullHullManifold|ShapePair`,
  golden `hullmanifold`, fixture 952) landed 2026-10-03, bit-exact. **P2 is
  complete** for convex shapes; meshes, height fields and compounds are P5.
- **P3 — broad phase.** Dynamic tree (insert, remove, rebuild, queries,
  ray casts) and pair finding.
  **Data structures landed 2026-10-04, bit-exact:** the dynamic tree (insert,
  remove, move, enlarge, rotations, the median-split rebuild, refit, moved
  flags, and the box, ray, box-cast and closest queries as explicit
  traversal states instead of callbacks), the pair hash set, the bit set,
  the id pool and qsort (`lib/physics/broadPhase/`, `container/`, golden
  `broadphase`, fixture 953). **Pair finding** (`b3UpdateBroadPhasePairs`:
  moved-sibling gathering, self and cross pair tasks, `b3ShouldCreatePair`'s
  filters) needs the world's shapes, so it is queued with P4.
  **Pair finding landed 2026-10-04 (P3b), bit-exact:** the moved-sibling
  self pairs, the cross-tree seeds and cross pairs, the batched pair-set cull
  and b3ShouldCreatePair, the sorted keys feeding contact creation, run block
  by block as worker 0 (`lib/physics/dynamics/BroadPhasePairs`,
  `ParallelFor`; golden `pairs`, fixture 956). Still open: compound pair
  emission (with compounds, P5), the custom filter callback (P8 decides its
  form); the joint check of b3ShouldBodiesCollide landed with P6a.

- **P4 — the first world.** Bodies, shapes, contacts, solver sets, islands and
  sleep, constraint graph, scalar contact solver, sensors and events; the
  falling sphere, restitution, friction and pyramid scenes bit-exact.
  **P4a landed 2026-10-04, bit-exact:** the world with its id pools, solver
  sets and islands, bodies (create, destroy, mass data, every getter and
  setter, forces and impulses, waking), shapes (sphere, capsule, hull,
  transformed hull; proxies in the broad phase) and the broad phase's proxy
  layer (`lib/physics/dynamics/`, `broadPhase/BroadPhase`, golden `world`,
  fixture 954). Split for the rest: **P4b** contacts (pair finding is done,
  P3b), the constraint graph and its colours, contact
  islands; **P4c** the scalar wide solver and contact solver, the stage
  list and dispatcher, and the 600-step scenes; **P4d** sleep and island
  splitting, sensors, contact and sensor events.
  **P4b landed 2026-10-04, bit-exact:** contacts (create, destroy, the
  convex manifold update with impulse carry-over, contact recycling), the
  constraint graph with Box3D's colour assignment and overflow, contact
  islands (link, unlink, merge), the contact moves of waking and of
  b3TrySleepIsland, and the narrow phase of the step with its state-change
  processing (`lib/physics/dynamics/Contact`, `ConstraintGraph`,
  `WorldGraph`, `WorldContacts`, `WorldIslands`, `Collide`; golden
  `contacts`, fixture 957).
  **P4c landed 2026-10-04, bit-exact:** the step (`stepWorld`), the solver
  with its stage list, blocks and sync indices, a single-threaded stage
  dispatcher module (`StageDispatcher`), the wide contact solver on the
  scalar path (a wide float is a struct of four Floats) and the scalar one
  for the overflow colour, integration, body finalization and continuous
  collision (`lib/physics/dynamics/WorldStep`, `Solver`, `FinalizeBodies`,
  `StageDispatcher`, `StepContext`, `FloatWide`, `ContactConstraints`,
  `ContactSolver`, `ContactSolverWide`, `ContactSolverWideSolve`,
  `BodyIntegration`, `Continuous`; golden `scenes`, fixture 958: a falling
  sphere, restitution, friction on a 20-degree slope and a 10-level pyramid,
  600 steps each, sleep off).
  **P4d landed 2026-10-04, bit-exact:** sleep with island splitting, sensors
  (overlaps, begin/end events, continuous sensor hits, destruction) and the
  world's contact, hit, sensor and body events (`WorldIslands`, `Sensors`,
  `SolverEvents`, `Events`, `WorldStep`; golden `events`, fixture 959). **P4
  is complete**; joints are P6.
  **P4e landed 2026-10-04, bit-exact:** the API an engine layer calls
  between steps: body type changes (b3TransferBody between solver sets,
  contacts destroyed, proxies recreated, islands), disable/enable, set awake,
  sleep enable and threshold, bullet, motion locks, body and shape contact
  data, sensor overlaps, shape filter (b3ResetProxy), friction, restitution,
  surface material, density, event flags, and every world setting
  (`WorldBodyChanges`, `WorldShapeChanges`, `WorldSettings`; golden `api`,
  fixture 969). Their joint steps landed with P6a.
- **P5 — meshes, height fields, compounds** and their manifolds. Split:
  **P5a** the triangle manifolds (triangle_manifold.c); **P5b** meshes (the
  BVH build, then mesh_contact.c, the mesh shape and its shape pairs, a
  scene);
  **P5c** height fields; **P5d** compounds.
  **P5a landed 2026-10-04, bit-exact:** b3ClosestPointOnTriangle and a
  triangle against a sphere, a capsule (with the simplex cache) and a hull
  (with the SAT cache, back-side hysteresis and the GJK fallback);
  b3LocalManifold gained its triangle fields (`lib/physics/collision/
  TriangleManifold`, `TriangleHullManifold`, `Manifold`; golden `triangle`,
  fixture 970).
  **P5b's mesh build landed 2026-10-04, bit-exact:** b3CreateMesh (welding,
  the BVH with the binned SAH, median and half splits, the depth-first
  triangle order, the edge flags), the generated grid, box, hollow-box and
  platform meshes, b3GetHeight, b3QueryMesh (with the scalar bounds and
  triangle-box tests) and b3ComputeMeshAABB (`lib/physics/geometry/Mesh`,
  `MeshQuery`; golden `mesh`, fixture 971). The mesh's byte-block layout
  and hash are not ported (a MeshData is a struct of lists). The mesh ray,
  shape and mover casts are P7.
  **P5b2 landed 2026-10-04, bit-exact:** mesh_contact.c
  (b3ComputeMeshManifolds: the cached triangle query, the per-triangle
  sphere, capsule and hull manifolds, the ghost-collision reduction over the
  found edges and vertices, clustering, b3CullPoints / b3ReduceCluster and
  the impulse matching), the mesh shape (b3CreateMeshShape and its box,
  centroid and extent cases; no mass) and the mesh cases of contact.c and
  the narrow phase (`lib/physics/dynamics/MeshContact`, `MeshCluster`;
  golden `meshscenes`, fixture 974). Per-triangle materials came with P5d,
  the triangle time of impact with P5e.
  **P5c landed 2026-10-04, bit-exact:** height_field.c (b3CreateHeightField
  with the 16-bit quantization and the edge convexity flags, holes, both
  windings, b3GetHeightFieldTriangle / Material, b3ComputeHeightFieldAABB,
  b3QueryHeightField, b3CreateGrid, b3CreateWave), the height shape
  (b3CreateHeightFieldShape, static bodies only) and the height-field cases
  of mesh_contact.c (`lib/physics/geometry/HeightField`; goldens
  `heightfield`, `heightscenes`, fixtures 979 and 980). The byte-block
  layout and hash are not ported; the ray, shape and mover casts are P7.
  **P5d landed 2026-10-04, bit-exact:** compound.c (b3CreateCompound with
  the shared material table and the child tree, b3GetCompoundChild,
  b3ComputeCompoundAABB, b3QueryCompound), the compound shape
  (b3CreateBakedCompoundShape, static only; mass none, as Box3D), the
  compound case of b3UpdateContact with b3UpdateConvexContact's flip, the
  per-triangle materials of mesh contacts, the compound pair emission of
  broad_phase.c (b3EmitCompoundPairs, checkCompounds) and b3Body_SetType's
  refusal for compound and height bodies (`lib/physics/geometry/Compound`;
  goldens `compound`, `compoundscenes`, fixtures 981 and 982).
  **P5e landed 2026-10-04, bit-exact:** the mesh, height-field and compound
  paths of b3ShapeTimeOfImpact (b3MeshTimeOfImpactFcn with its centroid
  early-outs and the small-sphere fallback, b3CompoundTimeOfImpactFcn with
  b3MakeCompoundChildSweep, b3ComputeSweptShapeAABB): continuous collision
  no longer skips those targets (`lib/physics/dynamics/TriangleTimeOfImpact`;
  golden `ccdscenes`, fixture 983).
  The compound pair emission (landed with P5d) has its own oracle case:
  tools/box3d-oracle/drivers/pairs.c adds a world of static compounds among
  a moving crowd, some refusing by filter (fixture 956).
- **P6 — joints**, one module per kind, each scene bit-exact.
  **P6a landed 2026-10-04, bit-exact:** the joint core (creation and
  destruction, graph colouring and overflow joints, joint islands with
  link/unlink, split and the merge of two sleeping sets, the joint moves of
  wake, sleep, b3Body_SetType, disable/enable and body destruction, the
  joint walk of b3ShouldBodiesCollide, the solver's joint stages and joint
  events) and the distance, revolute and filter joints (`WorldJoints`,
  `WorldJointSets`, `JointSolver`, `DistanceJoint`, `RevoluteJoint`; golden
  `joints`, fixture 984). Next: **P6b** prismatic and wheel, **P6c**
  spherical, motor, parallel and weld.
  **P6b landed 2026-10-04, bit-exact:** the prismatic joint (spring, motor,
  translation limits, locked rotation, point-to-line) and the wheel joint
  (suspension spring and limits, spin motor, steering spring and limits)
  (`PrismaticJoint`, `WheelJoint`; scenes `prismatic` and `car` of golden
  `joints`, fixture 984).
  **P6c landed 2026-10-04, bit-exact:** the spherical joint (spring, motor,
  cone and twist limits), the motor joint (spring and velocity control), the
  parallel joint and the weld joint (rigid or soft) (`SphericalJoint`,
  `MotorJoint`, `ParallelJoint`, `WeldJoint`; scenes `spherical`, `motor`,
  `parallel` and `weld` of golden `joints`, fixture 984). **P6 is complete.**
- **P7 — queries and the mover**: ray/shape casts, overlaps, the character
  loop (walls, steps, slopes).
  **P7a landed 2026-10-04, bit-exact:** the world queries (closest and
  all-hit ray casts, shape casts, box and shape overlaps, mover collide and
  cast; the shape dispatch for spheres, capsules and hulls), mover.c
  (b3SolvePlanes, b3ClipVector) and the character loop of
  samples/mover.cpp in Rae (`WorldQueries`, `ShapeQueries`, `Mover`,
  `character/CharacterMover`; goldens `queries`, `character`, fixtures 985
  and 986). **P7b** adds the mesh, height-field and compound queries.
  **P7b landed 2026-10-04, bit-exact:** the mesh, height-field and compound
  ray casts, shape casts, overlaps and mover planes (`MeshCast`,
  `HeightFieldCast`, `CompoundQueries`; the world queries and the sensors
  now reach every shape kind; goldens `queries` and `character` extended,
  fixtures 985 and 986). **P7 is complete.**
- **P8 — the ECS layer**: `PhysicsWorld`, `RigidBody`, `Collider`,
  `CharacterBody`, push/step/pull/event systems, spawn/despawn
  (`docs/physics-design.md` §5).
  **P8 landed 2026-10-04:** `lib/physics/ecs` (the `Physics` resource, the
  components, the five fixed-step systems, spawn/despawn, the venue, the
  queries with entities; `docs/physics-design.md` §5, rewritten for the
  port), the mover's up axis, and the renames `Shape` → `PhysicsShape`,
  `MeshData` → `PhysicsMeshData` (program-wide type names clashed with
  lib/ui and lib/Mesh3d). Callbacks: none needed for the §0 needs
  (`docs/physics-design.md` §5.6). Fixtures 987 (despawn safety) and 988
  (two worlds, identical transforms).
- **P9 — performance**: the benchmark programs, profiling, the fixes of §6;
  then `parallelLoop` parallelism.
  **P9a (single-threaded, 2026-10-05):** `benchmarks/physics` ports
  upstream's large pyramid, many pyramids, joint grid and convex pile
  (`run.sh` builds the release profile, runs the oracle's C scalar and SIMD
  apps, best of three); the numbers and the remaining gap are §6.1. Rain is
  not ported: it needs the ragdoll (`shared/human.c`) and
  `b3CreateTorusMesh`.
- **P10 — demos**: the featured example `examples/122_physics_playground`
  (the field scenes: pyramid, dominoes, a bridge, the character mover, thrown
  balls; a performance HUD; instanced rendering), a performance pass that
  profiles it at stress scale against a frame budget, and the vehicle as one
  of its scenes (`docs/physics-design.md` §5.7, §7), windowed, on the visual
  gate.

Engine work the phases depend on is queued with them (2026-10-02): a
threading and performance audit before P1 that writes
`docs/physics-performance-plan.md` (what the physics example needs from the
engine, measured against Box3D's C build at 1-8 workers), a runtime TSan
gate, a worker pool with `parallelLoop` on real threads, and two design
questions for the maintainer — atomics and staged parallel work, and a
`Float4` SIMD type.

Not planned: recording/replay/snapshots, the external task scheduler hook.

## 8. Tracking upstream

Box3D is young and moves fast. The port pins one commit. Moving it is a task
of its own: diff upstream between the two commits, port the diff module by
module, regenerate the goldens with the oracle, and let the bit-exact fixtures
show every place the behaviour changed.

## 9. Open questions

1. The 1.5× performance target (§6): acceptable, or tighter/looser?
2. ~~`countTrailingZeros`: a compiler intrinsic or a Rae loop?~~ Settled in
   P0: the intrinsic, under the existing name `trailingZeros` (overloads per
   width), inline in `rae_runtime.h`.
3. Recording/replay stays out unless netcode needs it — confirm.
