# Box3D: port to Rae, or keep it in C? — research

Research date: 2026-10-01. Box3D was read at commit `9f998c8` (2026-09-27,
`github.com/erincatto/box3d`). Every number below was measured from that
checkout or from Rae on this machine, and each section says how. The plan this
document questions is `docs/physics-rae-port-design.md`: port all of Box3D to
Rae, run its scalar path single-threaded first, and track upstream by hand.

## Short answer

**Do not port Box3D now. Use the C library as it is, behind a Rae ECS layer,
and spend the porting effort on the Rae features that a port would need
anyway.** Revisit the port when those features exist, and even then port only
the parts where owning the code in Rae pays for itself.

The reasons, each worked out below:

1. **Most of Box3D's speed comes from SIMD and threads that Rae cannot express
   today.** On Box3D's own benchmarks, SIMD alone makes the solver 1.2–7×
   faster, and 8 threads add another 5.7–7.3×. Together that is 21–43× on
   stacks and piles. Rae has no SIMD type, no atomics, and `parallelLoop` still
   runs sequentially.
2. **Rae itself is not the obstacle: safe Rae runs a contact-solver kernel at
   C speed.** That holds when the list accessors are the narrowed forms
   (`if let … = copyAt` to read, `if let slot: mod T => modAt` to write). The
   shorter `copyAtDefault` / `set` spellings are 3.5–6× slower today. That is a
   compiler or library fix, not a language limit (§4). The obstacle is
   item 1: SIMD and threads.
3. **Box3D is changing fast.** All 49 commits are from the last 4.5 months. In
   the last 3 months, 73 of its source files changed by +9 527 / −5 284 lines;
   in the last month alone, 49 files by +5 124 / −3 599. A hand port pins one
   commit and then either falls behind or spends a person-week a month
   re-porting diffs.
4. **The game does not need Box3D's speed, but it does need Box3D's
   correctness.** A track-and-field game simulates tens to a few hundred bodies,
   not 5 000-box pyramids. For that, the C library is already fast enough on one
   thread. The risky part of a port is the subtle solver and collision code,
   where a porting slip shows up as jitter or tunnelling, not as a crash.
5. **Most of the ECS benefit is available without porting.** Bodies can be
   entities, a component can hold the Box3D body id, one system can step the
   world and write back transforms, and another can turn Box3D's polled event
   arrays into ECS events. None of that needs Box3D's internals to be Rae.

## 1. What Box3D is

| | measured |
|---|---|
| source (`src/` + `include/`) | 68 624 lines of C17 |
| public API headers | 7 389 lines; 429 + 110 + 19 `B3_API` functions in `box3d.h`, `collision.h`, `math_functions.h` |
| history | 49 commits, first on 2026-05-10, last on 2026-09-27 |
| churn, last 3 months | 73 files, +9 527 / −5 284 lines in `src/` + `include/` |
| churn, last month | 49 files, +5 124 / −3 599 lines |

Features (README): continuous collision, convex hulls, capsules, spheres,
triangle meshes and height fields, ray/shape casts and overlaps, sensors, a
character mover, six joint types with limits, motors and springs, island
sleep, cross-platform determinism (also independent of thread count,
`docs/faq.md`), and recording/replay.

## 2. The optimisations a port would have to reproduce

### SIMD

`src/core.h` selects one of three back ends: SSE2 on x86, NEON on ARM, and a
scalar fallback (`BOX3D_DISABLE_SIMD`) where a "wide float" is a struct of four
floats. All are 4 lanes wide. SIMD is not limited to the contact solver, as the
earlier design doc assumed. It is used in:

| file | uses of the wide types |
|---|---|
| `contact_solver.c` | 166 (`b3FloatW`): the convex and mesh contact solvers |
| `mesh.c` | 82 (`b3V32`): triangle-mesh collision |
| `simd.c` | 55: the shared kernels |
| `height_field.c` | 27 |
| `convex_manifold.c` | 26 |
| `hull.c` | 9 |
| `dynamic_tree.c` | 9: bounding-volume tree queries |

### Threads

Box3D has its own thread pool (`scheduler.c`: OS threads, a semaphore and
atomic task slots, up to 32 workers). It also accepts the application's own
task system through `enqueueTask` / `finishTask` callbacks. Almost the whole
step runs in parallel:

- broad-phase pair finding (`broad_phase.c`);
- narrow-phase collision for every contact (`physics_world.c`, "collide");
- sensors, body finalisation and continuous collision for fast "bullet" bodies;
- every solver stage: prepare, warm start, solve, relax, restitution and store
  impulses.

The solver can run in parallel because of **graph colouring**: constraints are
split into up to 24 colours, and no two constraints of one colour touch the
same dynamic body. Constraints that do not fit go to an overflow set that is
solved on one thread (`constraint_graph.h`). Workers move between stages
through atomics and spin-waits.

### Other design choices

The solver is built for cache behaviour as much as for SIMD: "solver sets"
keep awake and sleeping bodies apart, contacts are stored in solver-ready
arrays per colour, and memory comes from arena and block allocators. This
layout is the performance design. Re-expressing it as ordinary ECS component
tables would undo much of it. A port should keep Box3D's internal layout, which
means the internal layout would not be ECS anyway.

## 3. What those optimisations are worth

Box3D ships its own measurements: `benchmark/amd7950x_scalar`,
`amd7950x_sse2` and `m2air_neon`, total milliseconds for a fixed step count at
1–8 threads. From the AMD 7950X files:

| benchmark | steps | scalar, 1 thread | SSE2, 1 thread | SIMD gain | SSE2, 8 threads | thread gain | scalar 1t → SSE2 8t |
|---|---|---|---|---|---|---|---|
| convex_pile | 500 | 40 706 ms (81 ms/step) | 5 835 | 7.0× | 938 | 6.2× | 43× |
| large_pyramid | 200 | 5 027 ms (25 ms/step) | 1 483 | 3.4× | 235 | 6.3× | 21× |
| many_pyramids | 100 | 4 933 ms (49 ms/step) | 1 590 | 3.1× | 218 | 7.3× | 23× |
| junkyard | 500 | 31 501 ms (63 ms/step) | 12 295 | 2.6× | 1 976 | 6.2× | 16× |
| washer | 1 000 | 42 026 ms (42 ms/step) | 17 570 | 2.4× | 2 845 | 6.2× | 15× |
| rain | 400 | 2 541 ms | 2 063 | 1.2× | 361 | 5.7× | 7× |
| joint_grid | 100 | 1 526 ms | 1 312 | 1.2× | 186 | 7.1× | 8× |
| trees25 / 50 / 100 | 500 | 815 / 346 / 222 ms | 660 / 286 / 168 | 1.2–1.3× | 191 / 100 / 70 | 2.4–3.5× | 3–4× |

SIMD matters most for contact-heavy piles and stacks, and hardly at all for
joints and trees. Threads matter everywhere there is enough work. A port that
keeps only the scalar, single-threaded path starts **15–43× behind** shipping
Box3D on heavy scenes, and 3–8× behind on light ones.

## 4. How fast would a direct Rae port be?

Measured with a kernel shaped like Box3D's contact solver: 4 096 bodies,
16 384 contacts, 50 iterations of "relative normal velocity → impulse → clamp →
apply to both bodies", with the data laid out as one list per component. The
same algorithm was written three ways and built with `-O2`. All three produce
the same checksum.

| version | time |
|---|---|
| C, plain arrays | 3–4 ms |
| Rae, `copyAtDefault(index:)` to read, `set(index:value:)` to write | 21–22 ms |
| Rae, `if let … = copyAt(index:)` to read, `set(index:value:)` to write | 9–10 ms |
| Rae, `if let … = copyAt(index:)` to read, `if let slot: mod Float => modAt(index:)` to write | 3–4 ms |
| Rae, raw buffer access in `unsafe` (`rae_ext_rae_buf_get` / `rae_ext_rae_buf_set` on `.data`) | 3 ms |

(The release profile, `--profile release`, gives the same numbers.)

So **safe, bounds-checked Rae is as fast as C** when the list accessors are
the narrowed forms, which the C backend lowers to a bare length check and a
load or store. This matches the earlier `benchmarks/list_access` suite, whose
read-only cases all use those forms. The slow spellings are `copyAtDefault`
(about 13 ms of the 19 ms gap) and `set` (about 6 ms). Both are still ordinary
function calls: `copyAtDefault` builds an optional through `copyAt`, and `set`
carries a warning path. The suite now has this kernel as its "Scatter update"
card (C, Rust, JavaScript, Python and the three Rae spellings).

A direct port written with the narrowed forms would therefore match Box3D's
scalar path. Written with `copyAtDefault` / `set`, it would run 3.5–6×
behind it. Either way, it starts 15–43× behind shipping Box3D on the heavy
benchmarks until SIMD and threads exist (§3).

## 5. What Rae would need before a port could be fast

1. **Make every safe accessor as fast as the narrowed forms.** Lower
   `copyAtDefault` / `copyAtFallback` and `set` the way `if let … = copyAt` and
   `modAt` already are: a length check plus a load or store, with no optional
   value and no out-of-line call. Then the obvious spelling is also the fast
   one. Today it costs 3.5–6× (§4). This helps every hot loop in the engine,
   not only physics.
2. **A 4-wide float type** (`Float4`) that lowers to SSE2 / NEON / wasm SIMD,
   with the scalar struct as fallback. This is exactly Box3D's own design.
   Without it, the contact-heavy scenes stay 2.4–7× behind.
3. **Real threads under `parallelLoop`, plus atomics or stage barriers.**
   `docs/concurrency-model.md` already plans this, but today `parallelLoop`
   runs sequentially and Rae has no atomic type. Box3D's per-stage worker
   synchronisation needs both.
4. **`-ffp-contract=off` in every build**, for Box3D's cross-platform
   determinism. This is already the port plan's P0.

Items 1–3 are general engine work. The renderer, the UI and the ECS benefit as
much as physics would.

## 6. Cost of a port, and of keeping it current

- **Up-front:** about 45 000 lines of C translated by hand, per the existing
  design (68 624 total, minus samples, recording and threading). Box3D also has
  18 000 lines of C tests that would need porting or replaying through a
  side-by-side check against the C build. That is a large multi-month effort
  even with agents doing the translation.
- **Ongoing:** at the measured churn of about 5 000 changed lines a month, each
  upstream update is a re-port of real solver and collision changes. The
  realistic choices are a permanent fork that stops following upstream, or a
  standing maintenance cost. A fork means fixing Box3D's own bugs, without its
  author, in code we did not design.
- **Risk:** a physics engine fails quietly (jitter, tunnelling, lost contacts,
  bad sleep), not with crashes. Determinism helps: the existing plan's
  side-by-side check against the C build is the right tool. But it only proves
  "same as the pinned commit", forever.

## 7. What owning the physics in Rae would actually buy

The real advantages of a Rae-native engine:

- **One language and toolchain.** No C library to build per platform. However,
  Rae already compiles C (its whole runtime is C), and Box3D is portable C17
  with no dependencies, so this saving is small.
- **Callbacks.** Custom contact filtering, pre-solve, and friction/restitution
  mixing take function pointers. Rae cannot pass a Rae function to C today. But
  these are optional: events are polled arrays (`b3World_GetContactEvents`,
  `GetSensorEvents`, `GetBodyEvents`), and the common ray query has a
  callback-free form (`b3World_CastRayClosest`).
- **Debuggability and changeability.** Engine code in the same language as the
  game can be read, stepped through and changed without crossing the C
  boundary. Real, but mostly useful once someone needs to change the solver.
- **Dogfooding the language.** A physics engine is an excellent stress test for
  Rae's performance features (§5). But that argument says: build those features
  — not necessarily by porting 45 000 lines first.

What it does **not** buy: a tighter ECS fit for the solver. Box3D's internal
layout is its performance, so a port keeps the layout and exposes it to the
ECS through a component and systems. That is the same integration the C
library gets through a binding layer.

## 8. The options

| | A. Full Rae port (current plan) | B. Box3D in C, Rae ECS layer | C. B now, port selected parts later |
|---|---|---|---|
| time to working physics in the game | months | days to weeks | days to weeks |
| speed | scalar C speed at best (with the narrowed accessors); SIMD and threads only after §5 | full SIMD + threads | full, unchanged |
| upstream fixes and features | manual re-port | update the vendored copy | same as B; ported parts are forks |
| C written and maintained by us | none | none (Box3D is upstream code; we write the binding) | none |
| ECS integration | systems around a Rae engine | the same systems around a C engine | the same |
| callbacks (custom filter, pre-solve) | available | not until Rae can pass functions to C | same as B |
| language features it forces | §5, all at once, before it is usable | none | §5 gradually, for the whole engine |

How option B would look:

- Vendor Box3D at a pinned commit and compile its `src/` with the program like
  the runtime's other C, with `-ffp-contract=off` and SIMD enabled.
- Generate low-level bindings from `include/box3d/*.h` with the existing
  C-header binding generator (`compiler/src/bindgen.c`). It currently
  recognises the WebGPU header style, so it would be extended for Box3D's
  `B3_API` declarations and plain C structs. This is the same pattern as
  `lib/webgpu`.
- Write the integration in Rae, in `lib/physics/`:
  - a `PhysicsBody` component holding the `b3BodyId`;
  - spawn and despawn systems that create and destroy bodies with their entities;
  - a step system on the existing fixed-timestep seam (`examples/114_walker_character/physicsSystem`);
  - a write-back system from body poses to `Transform3D`;
  - systems that copy contact, sensor and body events into ECS events;
  - query helpers for rays and overlaps.
- Use Box3D's own scheduler with a worker count, or later hand it Rae's task
  system through `enqueueTask` once Rae has real threads.

## 9. Recommendation

**Choose C: integrate Box3D's C library now behind a Rae ECS layer, and
re-plan the port as language work first.**

1. Replace the port phases P0–P10 in `QUEUE_PHYSICS.md` with:
   - (a) vendor Box3D and extend the binding generator to it;
   - (b) the `lib/physics` ECS layer and a 114 demo;
   - (c) the same memory and determinism checks the other examples have.
2. Queue the three language features from §5 as engine tasks in their own
   right, each measured with the kernel from §4:
   - fast safe accessors first, so `copyAtDefault` / `set` cost what the narrowed forms cost;
   - then `Float4`;
   - then real `parallelLoop` threads with atomics.
3. Revisit porting when the game needs something the C library makes hard —
   most likely a custom callback, or a solver change of our own. Then port that
   subsystem: the character mover, the queries, or one joint type. Do not port
   the whole engine, and do not try to follow Box3D upstream with a hand port.

The case for a full port only becomes good when all of these hold: §5 is done,
the game needs to change solver internals, and we are willing to own a
permanent fork. None of them hold today.

## Appendix: how the numbers were produced

- Box3D: cloned from `github.com/erincatto/box3d` (full history, 49 commits).
  - Line counts: `wc -l` over `src/` and `include/`.
  - Churn: `git diff --stat` from the last commit before 2026-06-30 and before
    2026-08-31 to `HEAD`, limited to `src/` and `include/`.
  - SIMD use: counts of `b3FloatW` / `b3V32` per file that includes `simd.h`.
  - Parallel work: the `b3ParallelFor` call sites and the solver stage enum.
  - Benchmarks: the CSVs under `benchmark/`, with step counts from
    `benchmark/main.c`.
- Rae vs C kernel: one program in five versions (C; Rae with
  `copyAtDefault` + `set`; Rae with `if let` + `set`; Rae with `if let` +
  `modAt`; Rae with raw buffer access), each run three times. C built with
  `gcc -O2`; Rae emitted with `rae build --emit-c` (compiler 0.1.98) and built
  with `gcc -O2`. Same machine (Apple Silicon), same checksum.
