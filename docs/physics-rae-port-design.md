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
  `ctz` needs a Rae `countTrailingZeros(UInt64)` (a compiler intrinsic over
  `__builtin_ctzll`, or a Rae loop if the intrinsic is not wanted — decided in
  task P0).
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

**Build flag first.** Rae compiles to C and today passes no
`-ffp-contract=off`. Clang on arm64 contracts `a * b + c` into a fused
multiply-add by default, so the same Rae program already gives different
float results on Apple silicon and x86. Box3D's cross-platform determinism
rests on that flag; the Rae build (native, `--emit-c` recipes, wasm) gets
`-ffp-contract=off` for every program, with a fixture that fails if a
contracted result appears. Cheap, and right for every Rae program, not only
physics.

**Bit-exact differential testing.** With the flag off and the scalar path,
the Rae port performs the same float operations in the same order as the C
scalar build, so it should produce **the same bits**, not just close values.
That turns "is the port right?" into a mechanical check:

- `tools/box3d-oracle/` — a script that clones Box3D at the pinned commit into
  a cache directory (outside the repository), builds it scalar
  (`BOX3D_DISABLE_SIMD`, `-ffp-contract=off`, `workerCount = 1`) together with
  a small C driver, and writes golden traces: per module (GJK distances,
  manifolds, hull construction, tree queries) and per scene (body transforms
  as hex floats every step).
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
  order. What the engine still lacks for that (a pool, real
  `parallelLoop`, atomics or stage barriers) is the subject of the threading
  and performance audit task, which comes before the solver is written.

## 7. Phases (queue tasks)

Each phase lands green: the full suite, its oracle fixtures bit-exact, and no
C added.

- **P0 — prerequisites.** `-ffp-contract=off` in every Rae build with a
  fixture; `countTrailingZeros(UInt64)`; the oracle tooling
  (`tools/box3d-oracle/`) with its first golden (the math module); a short
  measurement of `List.modAt` in a tight loop versus a raw buffer, to know
  early whether §6's unchecked access is needed.
- **P1 — math.** The math layer, bit-exact against the oracle.
- **P2 — convex geometry and narrow phase.** AABB, sphere, capsule, hull
  (quickhull), GJK distance and shape cast, manifolds for convex pairs.
- **P3 — broad phase.** Dynamic tree (insert, remove, rebuild, queries,
  ray casts) and pair finding.
- **P4 — the first world.** Bodies, shapes, contacts, solver sets, islands and
  sleep, constraint graph, scalar contact solver, sensors and events; the
  falling sphere, restitution, friction and pyramid scenes bit-exact.
- **P5 — meshes, height fields, compounds** and their manifolds.
- **P6 — joints**, one module per kind, each scene bit-exact.
- **P7 — queries and the mover**: ray/shape casts, overlaps, the character
  loop (walls, steps, slopes).
- **P8 — the ECS layer**: `PhysicsWorld`, `RigidBody`, `Collider`,
  `CharacterBody`, push/step/pull/event systems, spawn/despawn
  (`docs/physics-design.md` §5).
- **P9 — performance**: the benchmark programs, profiling, the fixes of §6;
  then `parallelLoop` parallelism.
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
2. `countTrailingZeros`: a compiler intrinsic (one line of C in the runtime,
   like the other math builtins) or a Rae loop? The intrinsic is recommended;
   it is the only C line this plan adds.
3. Recording/replay stays out unless netcode needs it — confirm.
