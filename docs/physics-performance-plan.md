# Physics performance plan — what the physics example needs from the engine

Written 2026-10-02, before the solver is ported (P4), so that the port's data
layout and step stages are parallel-ready from the start. It answers four
questions with measurements: how fast the featured physics example must be,
what Box3D itself needs to get there, what Rae is missing, and in what order to
build it. The port's own design is `docs/physics-rae-port-design.md`; the
concurrency model is `docs/concurrency-model.md`.

## 1. The target

The featured example (`examples/122_physics_playground`) steps its scenes inside
a frame with room left for rendering:

- **physics ≤ 4 ms per frame** at stress scale on the maintainer's machine
  (Apple M1 Max, 8 performance + 2 efficiency cores): a ~5 000-box pyramid, a
  joint grid, a pile of ~1 000 tumbling convex bodies;
- a fixed 60 Hz step with 4 substeps (Box3D's defaults), smooth on a 120 Hz
  display.

## 2. What it costs in C: Box3D measured at 1, 2, 4 and 8 workers

Box3D's own `benchmark/main.c` at the pinned commit (9f998c8), built by the P0
oracle cache (`~/.cache/rae/box3d-oracle`) twice: scalar (`BOX3D_DISABLE_SIMD`)
and SIMD (NEON on this machine). Both with `-ffp-contract=off`, validation off,
Release. Milliseconds **per step** (60 Hz, 4 substeps), best of 3, machine at
load ~6 from unrelated work. The single-thread SIMD numbers agree with the M2
Air results Box3D ships (`benchmark/m2air_neon`: large pyramid 9.1 ms per step
there, 8.1 ms here).

| scene (bodies) | scalar 1 | scalar 2 | scalar 4 | scalar 8 | SIMD 1 | SIMD 2 | SIMD 4 | SIMD 8 |
|---|---|---|---|---|---|---|---|---|
| large_pyramid (5 050 boxes) | 11.83 | 6.65 | 3.82 | 3.44 | 8.08 | 4.71 | 2.75 | 2.27 |
| many_pyramids (10 780 boxes) | 22.20 | 12.32 | 6.96 | 5.91 | 16.43 | 8.71 | 5.03 | 3.89 |
| joint_grid | 11.64 | 6.55 | 3.60 | 3.23 | 11.63 | 6.44 | 3.48 | 2.75 |
| convex_pile | 13.78 | 7.57 | 4.29 | 3.11 | 11.64 | 6.45 | 3.67 | 2.81 |
| washer (tumbler-like) | 22.44 | 12.16 | 7.22 | 5.66 | 18.73 | 10.22 | 6.24 | 4.88 |
| rain | 4.95 | 2.99 | 1.80 | 1.54 | 4.65 | 2.92 | 1.77 | 1.47 |
| spinner (mesh) | 4.25 | 2.85 | 2.02 | 2.07 | 3.63 | 2.36 | 1.68 | 1.86 |

What threads and SIMD are worth:

- **Threads are worth 3.1-3.3x at 4 workers and 3.4-4.4x at 8** on every
  contact- or joint-heavy scene (rain 2.75x / 3.2x, spinner levels off at 4).
  Beyond 4 the gain shrinks; with 8 performance cores and a loaded machine, 8
  workers is the useful ceiling here.
- **SIMD is worth 1.35-1.5x on contact-heavy scenes** (pyramids), 1.1-1.2x on
  piles and the washer, and **nothing on joints** (joint_grid 1.00x
  single-threaded: joints are solved scalar in Box3D too).
- **Neither alone meets the target in C.** The 5 000-box pyramid is 11.8 ms
  scalar and 8.1 ms SIMD single-threaded. It fits 4 ms only with at least 4
  workers: 3.8 ms scalar (no margin) or 2.75 ms with SIMD.

## 3. What that means for the Rae port

The port runs at `k` times Box3D's scalar C. P0 measured the access pattern
that dominates the solver at **k ≈ 1.0** (`benchmarks/solver_struct`: 0.98-1.02x
plain C on 80-byte constraints, `docs/physics-rae-port-design.md` §6); the
port's own target is k ≤ 1.5 (design §6, open question 1). Projected large
pyramid per step:

| | k = 1.0 | k = 1.2 | k = 1.5 |
|---|---|---|---|
| 1 thread, scalar | 11.8 | 14.2 | 17.7 |
| 4 workers, scalar | 3.8 | 4.6 | 5.7 |
| 8 workers, scalar | 3.4 | 4.1 | 5.2 |
| 4 workers, Float4 | 2.75 | 3.3 | 4.1 |
| 8 workers, Float4 | 2.3 | 2.7 | 3.4 |

So:

1. **Threads are NEEDED.** No single-threaded Rae port reaches 4 ms on the
   5 000-box pyramid; Box3D's C cannot either.
2. **SIMD (`Float4`) is the margin.** With threads alone the target is met only
   if the port stays within ~1.05x of C. With `Float4` it holds up to k ≈ 1.4.
3. **The 1.5x target in the port design is too loose for the featured example**
   (5.7 ms at 4 workers scalar). Recommendation for design open question 1:
   aim for 1.2x, which P0's measurement says is realistic, and treat 1.5x as
   the failure line.
4. **10 000 bodies is beyond the 60 Hz budget even in C** (many_pyramids:
   3.9 ms SIMD at 8 workers, the washer 4.9 ms). The example's body-count slider
   should top out where the HUD shows the budget breaking, and say so — a
   showcase of real performance, not a staged one.

**Measured (P9a, P9a-2, 2026-10-05):** k = 1.32 on the large pyramid
single-threaded (15.56 ms against C scalar's 11.76), 1.32 on many pyramids,
1.33 on the joint grid (2.58 before the joint layout fix) and 2.50 on the
convex pile. The causes and the follow-ups are in
`docs/physics-rae-port-design.md` §6.1; `benchmarks/physics/run.sh`
reproduces the table.

## 4. Box3D's step against Rae today

Box3D's step, in order (`physics_world.c`, `solver.c`): update the broad-phase
pairs (`b3ParallelFor` over moved proxies), collide (`b3ParallelFor` over
awake contacts), then the solver. The solver prepares the stage list once per
step and runs it with every worker at once:

`prepare joints → prepare wide contacts → prepare contacts → per substep:
[integrate velocities → warm start (per colour) → solve (per colour) →
integrate positions → relax (per colour)] → restitution (per colour) → store
impulses` (`b3SolverStageType`, 11 kinds).

With 4 substeps and typically 12-24 graph colours, **one step runs a few
hundred stage barriers**. Box3D does not re-dispatch work per stage: all
workers stay inside one task, claim fixed-size blocks of a stage with a
per-block atomic compare-and-swap on a monotonically growing sync index,
signal completion with an atomic counter, and spin (with a CPU `yield`/`pause`)
until the main thread publishes the next stage in one atomic word. The design
notes credit bepuphysics2. Per-worker state lives in `b3TaskContext` (an arena,
bitsets for contact/joint state changes and hit events, broad-phase pair keys,
sensor hits, a split-island candidate), merged in worker order after each
parallel section. The host can supply its own task system
(`b3WorldDef.enqueueTask` / `finishTask`); otherwise Box3D's `scheduler.c`
runs `workerCount` threads.

| Box3D needs | Rae today | gap |
|---|---|---|
| persistent worker threads (or a host task hook) | `spawn` = one new pthread per task, no pool | **needed**: a persistent pool |
| parallel-for over a range, dynamic chunk claiming (pairs, collide, sensors, prepare/store, transforms) | `parallelLoop` parses and runs **sequentially** | **needed**: `parallelLoop` on the pool |
| per-worker scratch (bitsets, pair keys, events) merged after the section | chunk scratch: the loop index is a chunk with its own slot, merged in chunk order — deterministic for any merge (decided 2026-10-03, fixture 946) | none |
| all workers resident across hundreds of stage barriers per step, spin-waiting | nothing; re-dispatching a pool task per stage would cost more than the stage | **needed**: a staged construct, or atomics plus a guaranteed-concurrent loop |
| atomics: ~50 call sites of load / store / fetch-add / compare-exchange on 32- and 64-bit ints | no atomic type | **needed** (or hidden inside the staged construct) |
| 4-wide floats (`b3FloatW`, NEON / SSE2 / wasm SIMD128) in the contact solver | no vector type | **margin**: `Float4` |
| a CPU pause/yield in spin loops | none | small runtime intrinsic, with the staged work |
| per-worker arena allocator | Lists and Buffers | not needed: pre-sized per-worker Lists; the steady step must allocate nothing |
| graph colouring, islands, sleep | — | plain data algorithms, ported as Rae |
| thread-safe runtime | `g_mem_*` counters are plain globals; the String temp pool is `__thread` (fine) | **needed**: the TSan audit |
| thread-safe ECS tables | ComponentTables are not thread-safe | not needed: the step never touches ECS tables (§6) |

One Rae constraint decides the shape of the staged construct: **Rae has no
function values**, so a library cannot take "the work for a block" as a
callback the way `b3ParallelFor` does. The parallel body has to be a block of
code the compiler outlines, as `parallelLoop`'s body is.

## 5. Engine features, in order

**Needed, in this order:**

1. **A thread-safe runtime and a TSan gate.** The `g_mem_*` counters and any
   other mutable static reachable from Rae code; a `make tsan` case. Small;
   blocks everything below.
2. **A persistent worker pool, with `parallelLoop` on it.** *(Landed
   2026-10-03: `docs/concurrency-model.md` §5. Per-worker scratch is chunk
   scratch — the loop index is a chunk with a contiguous slice and its own
   slot, merged in chunk order, deterministic by construction (fixture 946);
   no worker index. The concurrent mode moved to item 3. Launch 0.3-3.9 us back to
   back, 5-16 us from sleep; 3.8x at 4 workers, 6x at 8 on a compute kernel.)*
   - Workers = performance cores (`hw.perflevel0.physicalcpu` on macOS, 8
     here); the efficiency cores slow every spin barrier down to their pace.
     The calling thread participates.
   - Deterministic chunking: fixed-size chunks, results combined in chunk order.
   - The body sees its `workerIndex`, and may write the per-worker scratch slot
     of that index, besides the element of its own iteration.
   - Launch cost measured: the step has ~10 parallel-for launches outside the
     solver, so a launch must cost microseconds, not a thread start.
   - A mode that runs exactly one iteration per worker with all of them live at
     once (the solver's spin barriers deadlock if one worker waits for a thread
     that has not started).
3. **Staged parallel work for the solver** — decided and implemented
   2026-10-03 (`docs/parallel-stages-design.md`): a stage is a
   `parallelLoop`, with no atomics and no team construct; `unsafe` covers the
   colouring's disjoint body writes, checked at run time by
   `RAE_PARALLEL_CHECK`; the launch path is within ~1.0-1.4x of Box3D's own
   spin barrier. The text below is the original framing. The
   recommended shape, because Rae has no function values and the port should
   stay close to Box3D: an `Atomic(Int)` / `Atomic(Int32)` library type over C11
   `stdatomic` (load, store, fetchAdd, compareExchange; never on floats), a
   `cpuRelax()` intrinsic, and the guaranteed-concurrent `parallelLoop` mode of
   item 2. The solver then runs Box3D's own stage loop inside one parallel loop
   over workers, exactly as the C does. The alternative — a dedicated
   stage/barrier construct that hides the atomics — is smaller to misuse but
   larger to specify. Either is new surface and goes to the maintainer first.
4. **`Float4`** after the scalar contact solver exists (P4): lane-wise
   arithmetic, load/store from a `List(Float)`, compare/select. Box3D keeps its
   SIMD path bit-identical to the scalar one ("this math needs to match the SIMD
   version for cross platform determinism", `convex_manifold.c`; no approximate
   reciprocal square root in `contact_solver.c`), so the `Float4` contact solver
   is still checked bit-exact against the scalar oracle.

**Nice to have, measured before built:**

- Stepping physics on the workers while the main thread renders the previous
  step's transforms (double-buffered poses). This is the largest frame-time win
  at 120 Hz, but it needs the pool and a clear ownership story first.
- WASM threads (backlog #245), so the browser build is not single-threaded.
- A mixed-priority task system, futures, or work-stealing across unrelated
  tasks. Box3D needs none of them: parallel-for plus staged sync is the whole
  requirement.

**Not needed:** a thread-safe ECS, mutexes in Rae code, a general job graph.

## 6. The frame budget

At 60 Hz the frame is 16.7 ms; at 120 Hz it is 8.3 ms.

| system | budget at 5 000 awake boxes | notes |
|---|---|---|
| physics step | ≤ 4.0 ms | 4+ workers (§3) |
| ECS push (forces, kinematic targets) | ≤ 0.2 ms | single-threaded, before the step |
| ECS pull (body transforms into `Transform3D`) | ≤ 0.5 ms | only bodies that moved, from Box3D's body move events; sleeping bodies cost nothing |
| instance upload | ≤ 0.3 ms | 5 000 × 64 bytes = 320 KB per frame into one instance buffer |
| draws | one instanced draw per mesh kind, shadow cascades included | `lib/GbufferInstanced.rae` already issues one `DrawIndexed(instanceCount = N)`; no per-body draw call |
| rest of the frame (G-buffer, lighting, HUD) | the remainder | measured in the performance pass |

The physics step never runs inside an ECS system that other systems wait on:
push and pull are the only ECS contact, both single-threaded, before and after
the step. That is why the ECS tables need no locks.

At 120 Hz a 60 Hz step lands on every other frame: 4 ms of physics plus the
render must fit 8.3 ms on those frames, with transforms interpolated between
steps on the others. The alternative is a 120 Hz step with 2 substeps (about
the same solver work per second, twice the collision work). The performance
pass on the example measures both and picks one; the overlap of item 5's
nice-to-have removes the question.

## 7. The determinism rule

**Identical bits for any worker count, and for scalar versus `Float4`.** Box3D
is built for this, and the port keeps it:

- the graph colouring fixes the order in which constraints are solved,
  whatever thread solves them;
- per-worker results (bitsets, pair keys, events) are merged in worker-index
  order, never in finishing order;
- contacts are created in a deterministic order, so island links are too
  (`contact.c`);
- reductions over a `parallelLoop` combine fixed chunks in chunk order;
- atomics only count and claim work, never accumulate floats;
- no wall clock, no randomness in the step.

The checks: the oracle comparison at `workerCount = 1` (P1-P6), and in the
example a hash of every body transform after N steps that must be identical at
1, 2, 4 and 8 workers.

## 8. What this asks of the port now

Even single-threaded, P4 ports Box3D's step **structure**, not a simplified
one: solver sets, the constraint graph and its colours, the wide constraint
arrays, the stage list with its blocks and sync indices, and per-worker task
contexts (with one worker). The stage dispatcher is one small module whose
single-threaded version simply runs each stage's blocks in order; the
sync-index and completion-count fields are plain `Int`s behind a few functions
in that module until atomics exist. Threading the solver is then a change to
that module, not a restructuring of the solver.

## 9. How the numbers were produced

- Box3D at 9f998c8 in `~/.cache/rae/box3d-oracle/box3d` (P0's oracle cache).
  Two CMake builds, `bench-scalar` (`-DBOX3D_DISABLE_SIMD=ON`) and `bench-simd`,
  both `-DCMAKE_BUILD_TYPE=Release -DBOX3D_VALIDATE=OFF -DBOX3D_BENCHMARKS=ON`
  (Box3D's CMake adds `-ffp-contract=off`).
- `benchmark -t=8 -w=<1|2|4|8> -r=3 -b=<scene>`, best of 3 runs, total
  milliseconds divided by the scene's step count. Load average ~6 during the
  runs (unrelated headless browser tests), which mostly affects the 8-worker
  column.
- Rae's ratio to C: `benchmarks/solver_struct` (P0).
