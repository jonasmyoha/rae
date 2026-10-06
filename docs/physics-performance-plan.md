# Physics performance plan — what the physics example needs from the engine

Written 2026-10-02, before the solver is ported (P4), so that the port's data
layout and step stages are parallel-ready from the start. It answers four
questions with measurements: how fast the featured physics example must be,
what Box3D itself needs to get there, what Rae is missing, and in what order to
build it. The port's own design is `docs/physics-rae-port-design.md`; the
concurrency model is `docs/concurrency-model.md`.

## 1. The target

The physics playground (`examples/122_physics_playground_port`, and its track-B
twin `123_physics_playground_c`, `docs/physics-two-implementations.md`) steps its scenes inside
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

### 2b. The Rae port on the worker pool (P9b, 2026-10-05)

The port's step runs its parallel sites as `parallelLoop`s:
- each solver stage over its blocks;
- pair finding, collide, finalize, sensors and bullets over chunks of their
  blocks, one TaskContext per chunk, merged in chunk order.

Fixture 997 and the oracle fixtures replayed at 8 workers show the result is
identical at every worker count. Same scenes as above, measured interleaved
with Box3D's app on the same machine (load ~5-6 from other work), best of
3, ms per step. The Rae side runs at `RAE_WORKERS=N` with world workerCount
N, and uses the release profile's `Float4` lowering.

| scene | workers | C scalar | C SIMD | Rae | Rae / C scalar |
|---|---|---|---|---|---|
| large_pyramid | 1 / 2 / 4 / 8 | 11.73 / 6.61 / 3.79 / 3.40 | 8.30 / 4.79 / 2.85 / 2.66 | 12.92 / 6.72 / 3.92 / 4.09 | 1.10 / 1.02 / 1.03 / 1.20 |
| many_pyramids | 1 / 2 / 4 / 8 | 22.27 / 12.19 / 6.88 / 5.25 | 16.15 / 8.96 / 5.29 / 3.84 | 25.14 / 13.13 / 7.32 / 5.66 | 1.13 / 1.08 / 1.06 / 1.08 |
| joint_grid | 1 / 2 / 4 / 8 | 11.82 / 6.32 / 3.41 / 2.60 | 11.90 / 6.33 / 3.45 / 2.59 | 13.52 / 6.99 / 3.88 / 3.44 | 1.14 / 1.11 / 1.14 / 1.32 |
| convex_pile | 1 / 2 / 4 / 8 | 14.14 / 7.62 / 4.25 / 3.07 | 11.78 / 6.46 / 3.65 / 2.71 | 16.71 / 10.18 / 6.56 / 5.26 | 1.18 / 1.34 / 1.54 / 1.71 |
| rain | 1 / 2 / 4 / 8 | 5.01 / 2.97 / 1.78 / 1.52 | 4.94 / 2.93 / 1.80 / 1.52 | 6.23 / 4.59 / 4.11 / 5.56 | 1.24 / 1.55 / 2.31 / 3.66 |

What it shows:
- **The pyramids and the joint grid scale like Box3D.** They run 3.2-4.4x
  faster at 8 workers, against C's 3.4-4.5x. The 5 000-box pyramid is
  3.9 ms at 4 workers, inside the 4 ms budget (§1).
- **The convex pile scales 3.2x**, against C's 4.6x. Sampled at 8 workers,
  the workers mostly wait while the main thread runs the step's serial
  parts: contact creation from the new pairs, collide's state pass, the tree
  rebuild and refit, and the solver's setup. Box3D overlaps the tree
  rebuild with collide.
- **Rain stops scaling at 4 workers and is slower at 8.** At 8 workers most
  samples sit in the runtime's buffer alloc and free. The mesh narrow
  phase (`computeMeshManifolds`, `collideTriangleInto`, `queryMesh`)
  allocates per contact and per triangle. Every allocation also updates
  the runtime's always-on atomic counters, so the cores fight over one
  cache line. Both are queued.
- **Per-thread counters fixed the contention (2026-10-06).** Each thread
  now counts into its own counter block, and readers sum the blocks
  (`compiler/runtime/runtime_core_memory.c`). Rain, best of 3, measured
  interleaved with the old build (load ~5):

  | workers | 1 | 2 | 4 | 8 |
  |---|---|---|---|---|
  | before | 6.19 | 4.56 | 4.11 | 5.37 |
  | after | 6.26 | 3.68 | 2.39 | 1.94 |

  It now scales at every step up to 8 workers, though it is still 1.3x
  Box3D's 1.52 ms at 8. The per-contact allocations in the mesh narrow
  phase are still queued. `benchmarks/parallel_loop`, which does not
  allocate, did not change. The checksum is identical.
- **The mesh narrow phase no longer allocates (2026-10-06).** Each chunk's
  TaskContext now owns a `MeshScratch` (`dynamics/MeshContact.rae`), which
  holds the per-contact lists, the kept per-triangle manifold slots, the
  cluster pool (`MeshCluster.ClusterScratch`), the capsule-triangle proxies
  and segment (`TriangleManifold.TriangleScratch`), and the query lists
  (`queryMeshInto`, `queryHeightFieldInto`). The mesh time-of-impact sweep
  also borrows the chunk's proxy and query lists (`MeshImpactScratch`).

  Allocations per steady rain step, counted over steps 360-400 at 1 worker,
  fell from 29 216 to 209. What collide still allocates (about 48) is lists
  growing for newly created contacts: the triangle cache and the
  manifolds. The rest is outside collide: the solver's island sleep and
  wake copy whole contacts, and the scene's body spawning.

  Rain, best of 3, measured interleaved with the build before (load ~3):

  | workers | 1 | 2 | 4 | 8 |
  |---|---|---|---|---|
  | before | 6.16 | 3.68 | 2.36 | 2.04 |
  | after | 5.86 | 3.39 | 2.13 | 1.80 |

  At 8 workers that is 1.18x Box3D's 1.52 ms. Every benchmark checksum and
  fixtures 970-983, 996 and 997 are unchanged.
- **Sleep, wake and destroy no longer copy whole contacts (2026-10-06).**
  Walking a body's contact list read each contact with `copyAtDefault`,
  which deep-copied its manifolds and triangle cache. Those walks are in
  island sleep and wake, shape destruction and joint creation. They now
  read a `ContactLinks` record (`dynamics/Contact.rae`) that holds the
  edges, shape ids, set placement and flags. Rain allocations per steady
  step fell from 209 to 110. The rest is list growth for brand-new contact
  slots (a reused slot keeps its buffers, so reserving capacity would only
  move one allocation) and the scene's body and joint creation. Step time
  did not change, because sleep and wake are not hot.

### 2c. The step's serial parts at 8 workers (2026-10-06)

The convex pile was timed per section with temporary `nowNs()` timers
(µs per step, averaged over its 500 steps; the timers are not kept):

| section | 1 worker | 8 workers | |
|---|---|---|---|
| collide's narrow phase | 10 081 | 2 706 | parallel, 3.7x |
| solver stages | 2 789 | 1 302 | parallel, 2.1x |
| finalize bodies | 560 | 164 | parallel |
| pair finding | 1 982 | 485 | parallel |
| **dynamic/kinematic tree rebuild** | 377 | **397** | serial |
| collide's state-change pass | 221 | 248 | serial |
| contact creation from the sorted keys | 136 | 133 | serial |
| merging the moved proxies (applyMovedProxies) | 70 | 104 | serial |
| solve's colour and block setup | 42 | 54 | serial |
| refits | 25 | 28 | serial |
| pair-key gather and sort | 23 | 24 | serial |

The serial parts add up to about 1.0 ms of the 5.8 ms step. The largest is the
tree rebuild. As Box3D does (b3EnqueueTreeUpdate), it now runs beside the
narrow phase:
- `updateBroadPhasePairs` only marks it pending (`world.treeRebuildPending`).
- `finishTreeRebuild` runs it first in chunk 0 of collide's parallelLoop.
  Chunk 0 is the caller's range, so it starts at once, and the other workers
  steal the rest of that range. The narrow phase reads no tree.
- `solve` calls it again in case collide did not (a no-op). Fixture 956,
  which dumps the trees after `pairs.update`, also calls it.

Giving the rebuild its own 33rd iteration was slower (5.7 -> 6.8 ms): the
pool aims for 4 chunks per worker, so 33 iterations are paired into 17
chunks, which balance worse.

At 8 workers, measured interleaved with the build before (load ~4-6):

| scene | before | after |
|---|---|---|
| convex_pile | 5.19 | 4.73 |
| large_pyramid | 3.76 | 3.55 |
| many_pyramids | 7.58 | 7.32 |
| joint_grid | 4.27 | 4.33 |
| rain | 1.82 | 1.67 |

The convex pile now scales 3.5x (16.5 -> 4.73 ms), against Box3D's 4.6x.
The checksums are identical at 1, 2, 4 and 8 workers. Fixture 997, the
oracle scene fixtures replayed with 8 workers forced, and the TSan gate
pass.

What is left:
- The two parallel sections scale worst: collide's narrow phase reaches
  3.7x (the ideal is 1 260 µs, not 2 706) and the solver stages 2.1x.
- The rest of the serial parts (state pass, contact creation, the
  moved-proxy merge) are serial in Box3D too.

### 2d. Balancing collide's chunks (2026-10-06)

Timing each collide chunk of the convex pile at 8 workers (temporary
timers, µs per step) showed why its narrow phase scaled only 3.7x. Each
TaskContext slot took a contiguous run of the contact list's blocks. The
list holds every colour's touching contacts first and the cheap
non-touching ones last, so chunks 0-2 took ~1 850 µs each, chunk 3 668,
and chunks 4-31 ~170 each. The pool gives each worker a contiguous range of
4 chunks, so chunks 0-3 all landed on the caller, which also runs the tree
rebuild. Stealing could not even that out.

`collideChunk` now deals the blocks round-robin: chunk c takes blocks c,
c + chunkCount, and so on. The chunks then cost 170-490 µs, and the loop
fell from 2 866 to 2 147 µs per step. A chunk only sets bits in its own
state bitset (OR-ed afterwards) and uses its own scratch, so the result
does not depend on which chunk ran a block. The checksums are identical at
1, 2, 4 and 8 workers. Fixture 997, the oracle scene fixtures with 8
workers forced, and the TSan gate pass.

Measured interleaved with the build before (best of 3, load ~5-8):

| scene | 4 workers before / after | 8 workers before / after |
|---|---|---|
| convex_pile | 6.35 / 5.27 | 5.21 / 4.05 |
| large_pyramid | 3.67 / 3.61 | 2.96 / 3.00 |
| many_pyramids | 6.83 / 6.84 | 5.28 / 5.40 (noise: 6.0-9.2 over 5 runs each) |
| joint_grid | 3.85 / 3.84 | 3.33 / 3.19 |
| rain | 1.93 / 1.92 | 1.70 / 1.67 |

The convex pile now scales 4.1x at 8 workers (16.5 -> 4.05 ms), against
Box3D's 4.6x.

### 2e. The solver stages at 8 workers match Box3D's (2026-10-06)

§2c's "solver stages scale 2.1x" was measured with per-section timers on
an older build. It was compared against an assumed ~4x for Box3D, and that
assumption was wrong. Box3D's benchmark app records the solver-stage time
itself (`-s` writes `p.constraints` per step). On the convex pile, on this
machine, at the same moment:

| | 1 worker | 8 workers | speedup |
|---|---|---|---|
| Box3D scalar, `p.constraints` | 2.641 ms | 1.035 ms | 2.55x |
| Rae port, runSolverStages | 2.77 ms | 0.99-1.04 ms | 2.7-2.8x |

So the port's solver is already level with Box3D's, in both absolute time
and scaling. Per-stage timing (temporary: each block's time stored in its
own TaskContext slot and read after the stage) shows where the 8-worker
time goes:
- **The work itself inflates.** A colour stage's blocks sum to ~1.8x their
  1-worker time, because a body's state is written by one core in colour k
  and read by another in colour k+1. Body stages (integrate velocities)
  do not inflate: a block costs 5.3 µs at 8 workers, exactly its 1/32 share.
  This is inherent to the parallel colour sweep, which Box3D uses too.
- **The split is even.** A stage's blocks cost about the same, and the
  pool keeps each worker on the same blocks every stage.
- **Stragglers and the barrier.** An occasional block takes 2-4x longer on
  a random worker (the machine was at load 4-6), and the join costs
  ~1.8 µs (`benchmarks/parallel_stages`, against 3.0 for Box3D's own spin
  barrier). Across ~100 stages per step, these are most of the gap between
  the wall time and work / 8.

No change was made. The one lever left is locality: which worker solves
which constraints. It does not change results, because a colour's blocks
are independent. Keeping a body's constraints on one core across colours
would need a body-ownership-aware assignment that Box3D does not have
either, and that is a design question, not a fix.

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

**Measured (P9a to P9a-8, 2026-10-05):** k = 0.98 on the large pyramid
single-threaded (11.12 ms against C scalar's 11.31), 1.06 on many
pyramids, 1.13 on the joint grid and 1.20 on the convex pile (2.50 before
P9a-3); a steady large-pyramid step allocates nothing. The causes and the
follow-ups are in
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
     The calling thread participates. (Since §10h: performance cores minus
     one, 7 here, at the launcher's QoS class.)
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
nice-to-have removes the question. **Picked (§10f): the 60 Hz step with
interpolated poses.** A 120 Hz step with 2 sub-steps costs ~1.5x the CPU per
second and only lowers the per-frame peak, which does not matter while the
frame is GPU-bound.

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

## 10. The playgrounds' frame, measured (2026-10-06)

Measured with the playground's own per-system profile:
`RAE_PLAYGROUND_PROFILE=<frames>` times `<frames>` frames after 120 warm-up
frames, logs one `[profile]` report, and logs a `spike` line for any system
over 8 ms in one frame (`lib/physicsScenes/FrameProfile.rae`). The setup
was the pyramid at 5 000 boxes (~4 950 awake) with autofire, the pool's 8
workers, release build, M1 Max. Present ran without vsync
(`RAE_PRESENT_MODE=immediate`, new): the maintainer's main display is a
60 Hz 6K monitor, and with vsync, present simply waits out the 16.7 ms
interval, which hides whether a frame would fit 120 Hz. The window is
1280 x 800 points, 2560 x 1600 pixels. The machine was at load 4-8 from
other work. ms per frame, average and worst over 1 200 frames:

| system | budget (§6) | track A: Rae port | track B: Box3D C |
|---|---|---|---|
| input, camera | | 0.06 / 0.5 | 0.06 / 0.6 |
| spawn and despawn balls | | 0.00 / 0.03 | 0.00 / 0.03 |
| physics push (ECS -> world) | ≤ 0.2 | 0.02 / 0.15 | 0.02 / 0.09 |
| **physics world step** | **≤ 4.0** | **1.5-2.2 / 9-44 (spikes)** | **1.2-1.3 / 7-10** |
| physics pull (world -> ECS) | ≤ 0.5 | 0.02 / 0.3 | 0.02 / 0.08 |
| physics events | | 0.00 / 0.04 | 0.00 / 0.01 |
| keep previous poses | | 0.05 / 0.3 | 0.04 / 0.2 |
| interpolate poses | | 0.17 / 0.5 | 0.16 / 0.3 |
| HUD | | 0.02 / 0.09 | 0.02 / 0.06 |
| instance batches (fill) | ≤ 0.3 | 0.24 / 0.4 | 0.23 / 0.3 |
| shadow casters (cull, queue) | | 0.66 / 1.0 | 0.66 / 1.0 |
| gbuffer (instance upload, draw) | ≤ 0.3 | 0.28 / 0.6 | 0.28 / 0.7 |
| deferred passes | | 0.65 / 1.1 | 0.65 / 1.0 |
| UI overlay | | 0.26 / 0.5 | 0.26 / 0.45 |
| present (waits for the GPU) | | 7.9-8.2 / 35 | 7.9 / 25 |
| **whole frame** | **8.3 at 120 Hz** | **12.2-12.8** | **11.5-11.6** |

Per physics step (frames that stepped), the port averages ~2.1-2.8 ms and
Box3D ~1.7 ms. Both are inside the 4 ms budget.

What it shows:
- **The frame is GPU-bound, not physics-bound.** All the CPU work is
  ~4.5 ms (port) and ~3.6 ms (C). Present then waits ~8 ms for the GPU, so
  a frame takes ~12 ms: it misses 120 Hz (8.3 ms) and fits 60 Hz. Physics
  and the ECS (push, pull, events, poses: under 0.3 ms together) are not
  the problem. The deferred renderer has no per-pass GPU timing yet
  (`lib/GpuTiming.rae` is driven only by a check fixture), so which pass
  costs the GPU's 8 ms is the next measurement (queued).
- **With vsync** on the 60 Hz display, the frame is 16.67 ms and present
  waits ~12 ms: everything fits 60 Hz.
- **The port's step has spikes; Box3D's does not.** In the same runs, the
  port logged 1-30 frames with a single step of 8-28 ms. The spike
  breakdown puts the time in the solver stages: 4-15 ms against ~2 ms
  normally, with the same ~4 950 bodies and 7 colours awake. Box3D logged
  none. The count swung between runs with the machine's load. Letting the
  pool's workers spin for 20 ms instead of 250 µs between launches made it
  worse (worst step 82 ms): spinning workers then compete with the render
  thread and the driver. So the likely cause is preemption under CPU
  contention. The port syncs ~100 stage launches per step, and one
  descheduled worker stalls each barrier; Box3D's solver runs as one task.
  Fixed in §10h: the cause was the workers' QoS class and the pool taking
  every performance core, not the number of barriers.
- **Catch-up spirals.** A slow step makes the next frame owe 2-4 steps
  (`maxStepsPerFrame` is 4 in both tracks), which made the worst frames
  40-80 ms. The accumulator already drops time beyond the cap. Whether to
  cap lower is item (4) of the performance pass (queued).

### 10b. Against Box3D's own samples app (2026-10-06)

Box3D's samples app (its sokol renderer, from the oracle cache's source at
9f998c8) was built with a small timing patch: `tools/box3d-oracle/samples.sh`
applies `samples-timing.patch`, which sets the worker count and window size
and prints one line at exit. It ran "Benchmark / Large Pyramid" (sample 11,
Box3D's CreateLargePyramid: 5 050 boxes, sleeping off) at 8 workers in a
1280 x 800 window (high-DPI, 2560 x 1600 pixels, like the playground's), 1 200
frames after 120. The playgrounds ran the same pyramid (`RAE_PLAYGROUND_BODIES=
5050`, scene 1) with autofire, which keeps ~4 950 of the boxes awake (the
playground's world sleeps). Both had vsync; the runs were interleaved, two
rounds, load 5-7. ms:

| | physics step | CPU per frame (frame minus present/drawable wait) | GPU per frame | frame |
|---|---|---|---|---|
| Box3D samples app | 1.44-1.50 (Box3D's profile; 2.57 over frames 120-720 while the pile settles) | ~2.5 (its step 2.2-2.3 included) | ≤ 2.9 (it renders uncapped: 2.9 ms frames, 0.39 ms drawable wait) | 2.9 |
| track B: Box3D C through Rae's ECS | 1.13-1.28 | ~3.6 | ~8 (§10) | 16.67 (vsync) |
| track A: the Rae port | 1.24-1.47, spikes to 18-37 | ~3.7 | ~8 (§10) | 16.67 (vsync), worst 33-52 |

The gaps and their causes:
- **Physics is at parity on average.** With vsync both tracks step in about
  Box3D's time. Track B is a little faster than the samples app, because
  the sample's world never sleeps and the playground's does, and the
  samples app does more per frame inside its Step. The port's average is
  within noise of Box3D's. The steps are faster here than in §10's
  uncapped runs (2.1-2.8 ms per step), where the render thread and the
  driver competed with the workers all the time. Only the port's spikes
  remain (§10, queued). Track B logged 0-3 frames over 8 ms, all but one
  of them catch-up frames with 2 steps.
- **Rendering is the gap: the GPU first, then the CPU.** Box3D's app
  draws the same 5 050 boxes with shadows at 2560 x 1600 in under ~2.9 ms
  of GPU per frame. The playground's deferred renderer needs ~8 ms, about
  3x. That is why the playground misses 120 Hz and Box3D's app does not.
  On the CPU, the playground spends ~2.1 ms on rendering:
  - instance batches 0.24;
  - shadow casters 0.66, which culls and queues 5 000 casters per frame
    on the main thread;
  - gbuffer 0.28;
  - deferred passes 0.65;
  - UI 0.26.

  Box3D's sample spends ~0.3 ms beyond its step. Which GPU pass costs the
  8 ms needs the deferred renderer's per-pass GPU timing (queued). The
  per-frame CPU shadow-caster culling is the largest CPU render item.
- **The ECS layer costs nothing measurable:** push, pull, events and poses
  are under 0.3 ms per frame in both tracks (§10).

### 10c. Shadow casters from the instance batches (2026-10-06)

`queueShadowCasters` walked every body four times (once per mesh kind),
looked each one up in the transform table, rebuilt its model matrix and
appended 21 values to the shadow queue, one call per body. It now queues
each kind in one go from the instance batch `fillBatches` has just built
for the G-buffer (`drawShadowCasterBatchWith` -> `shadowQueueRecords`, which
copies each record's model). The queue ends up the same: one run per mesh,
in the same order, and every body still casts (the debug log's caster
count is bodies + live balls).

Temporary timers inside the shadow lap, port playground, 5 000 boxes,
autofire, ms per frame:

| | before | after |
|---|---|---|
| shadow lap (RAE_PLAYGROUND_PROFILE, both tracks) | 0.66 | 0.48-0.50 |
| of which queueing the casters | ~0.3 | 0.084 |
| beginShadowPass (cascade uniforms) | | 0.14 |
| endShadowPass: model upload (320 KB) | | 0.095 |
| endShadowPass: three cascade passes | | 0.022 |
| endShadowPass: its own queue submit and poll | | 0.18 |

What is left is fixed cost per frame, not per caster. The shadow pass
uploads the same model matrices the G-buffer's instance buffer already holds,
and submits on its own instead of in the frame's submission (queued).

### 10d. The shadow pass's submission and model upload: not worth sharing (2026-10-06)

§10c left two per-frame costs in the shadow lap: the pass's own queue
submission (~0.17 ms; polling is 0.003 ms) and the 320 KB model upload
(0.095 ms). This measured whether to remove them:
- **The submission.** The shadow cascades were recorded into the G-buffer
  pass's recording and submitted with it. The G-buffer kept the shadow
  pass's unsubmitted recording and opened its geometry pass in it.
  Measured with RAE_PLAYGROUND_PROFILE, port playground, 5 000 boxes,
  autofire, ms per frame:

  | | shadow lap | gbuffer lap | together |
  |---|---|---|---|
  | separate submissions (§10c) | 0.48-0.50 | 0.24-0.27 | 0.74-0.77 |
  | one submission | 0.33-0.37 | 0.37-0.42 | 0.70-0.79 |

  The cost moved with the work instead of disappearing. Committing a
  command buffer costs in proportion to what was encoded into it, so one
  submission instead of two saves nothing measurable. It was reverted: a
  carried recording across two modules is not worth ~0.03 ms.
- **The model upload.** Sharing the G-buffer's instance buffer would save
  at most the 0.095 ms upload. It would need a third shadow pipeline and
  shader that read the G-buffer's 160-byte draw records, and the shadow
  cascades encoded after the geometry pass, so the records' offsets are
  known. Static and skinned casters from other apps would keep the
  current path. Not done: too much machinery for 0.1 ms in a frame that
  is GPU-bound at ~8 ms (§10, §10b).

The trial also turned up a compiler bug, queued: `own` moving out of an
`opt` field (`cache.recording = own cache.carried`) copies the value
without emptying the source, so assigning the source afterwards frees what
the destination still holds. A `List` field is moved correctly (the
source is zeroed).

### 10e. Scaling curves: bodies and workers, both tracks (2026-10-06)

`benchmarks/physics_scaling` steps the playground's pyramid headless on both
tracks: `RAE_PLAYGROUND_SCALING=<counts>` in the playground itself, with
sleeping off, 20 settling and 100 timed 60 Hz steps (4 sub-steps). It
covers 1k to 20k bodies at 1, 2, 4 and 8 workers. The page is on the
devtools Benchmarks tab ("Physics scaling"). M1 Max, release, load ~4.5,
median ms per step:

| bodies | port 1 w | port 8 w | port speedup | Box3D C 1 w | Box3D C 8 w | Box3D C speedup | port / C at 8 w |
|---|---|---|---|---|---|---|---|
| 1 000 | 1.78 | 0.70 | 2.6x | 1.29 | 0.72 | 1.8x | 0.97x |
| 2 000 | 3.96 | 1.48 | 2.7x | 2.70 | 1.02 | 2.6x | 1.45x |
| 5 000 | 11.81 | 2.87 | 4.1x | 7.92 | 2.20 | 3.6x | 1.31x |
| 10 000 | 27.28 | 5.92 | 4.6x | 17.99 | 4.23 | 4.3x | 1.40x |
| 20 000 | 59.10 | 12.57 | 4.7x | 40.35 | 9.00 | 4.5x | 1.40x |

What the curves show:
- **Both tracks grow roughly linearly with bodies.** The 1-worker step is
  ~2x per doubling, a little more for the port at 20k. On a log-log chart
  the lines are parallel.
- **The port scales as well as Box3D across workers.** It reaches 4.7x
  at 20k bodies against Box3D's 4.5x, and 4.1x at 5k against 3.6x. Small
  pyramids stop scaling past 4 workers on both tracks, because there is
  too little work per stage (§2e).
- **The port's constant factor is ~1.3-1.5x Box3D's.** Track B's Box3D is
  the release build with its SIMD paths (NEON), so this matches §2's ratio
  of the port to Box3D SIMD on the large pyramid. The port's own Float4
  lowering applies only in its solver.
- **The 4 ms budget** at 8 workers holds to ~6 000 bodies on the port and
  ~9 000 on Box3D. 5 000 awake bodies (§6's target) is 2.9 ms on the port
  and 2.2 ms on Box3D.

### 10f. 120 Hz: the step rate, the window and async physics (2026-10-06)

Two new switches for this:
- `RAE_PLAYGROUND_WINDOW=x,y,w,h` places and sizes the playground window. The
  built-in 120 Hz panel sits left of the 60 Hz main monitor:
  `-1628,675,1280,800` opens on it.
- `RAE_PLAYGROUND_STEP=<hz>,<subSteps>` sets the fixed step, 60,4 by default
  (`physicsScenes/SceneTypes.playgroundStepRate`).

**The step rate.** Headless (`RAE_PLAYGROUND_SCALING`, sleeping off, 8
workers, best of three interleaved), ms per fixed step and per simulated
second:

| track, bodies | 60 Hz x 4 sub-steps | 120 Hz x 2 sub-steps | 120 Hz / 60 Hz per second |
|---|---|---|---|
| port, 5 000 | 3.38 (203 ms/s) | 2.62 (315 ms/s) | 1.55x |
| port, 10 000 | 6.84 (411 ms/s) | 5.20 (624 ms/s) | 1.52x |
| Box3D C, 5 000 | 2.49 (149 ms/s) | 1.78 (213 ms/s) | 1.43x |
| Box3D C, 10 000 | 4.90 (294 ms/s) | 3.58 (430 ms/s) | 1.46x |

A 120 Hz step costs ~1.5x the CPU per second: collision runs twice as
often, and halving the sub-steps does not halve the step. Its only gain is
a lower peak per frame: 2.6 ms every frame against 3.4 ms every other
frame.

**The frame on the 120 Hz panel.** Port playground, 5 000 boxes, autofire,
8 workers, RAE_PLAYGROUND_PROFILE, ms per frame:

| run | whole frame | present (waits for the GPU) | physics |
|---|---|---|---|
| 1280 x 800 points (2560 x 1600 px), no vsync | 11.3 | 7.2 | 1.8 |
| 960 x 600 points (1920 x 1200 px), no vsync | 7.6 | 3.9 | 1.5 |
| 1280 x 800, vsync at 120 Hz, 60 Hz step | 9.4 (~106 fps) | 5.9 | 1.3 |
| 1280 x 800, vsync at 120 Hz, 120 Hz x 2 step | 10.1 (~99 fps) | 5.9 | 1.9 |

- **A smaller window reaches 120 Hz.** At 1920 x 1200 px the frame
  averages 7.6 ms. The GPU cost follows the pixel count (~4.1 MP: ~11 ms;
  ~2.3 MP: ~7.6 ms), so the deferred renderer's full-screen passes are the
  bottleneck, not the 5 000 instances. At the default size the frame stays
  GPU-bound and misses 8.3 ms until the renderer is fixed (the per-pass GPU
  timing task, queued; a render scale would also do it).
- **The pick (§6): a 60 Hz step with interpolated poses.** The playgrounds
  already interpolate (PoseBlend). The 120 Hz step costs ~1.5x the CPU and
  made the frame slower (10.1 against 9.4 ms), because the frame is
  GPU-bound and the extra step work only adds to it. Its lower per-frame
  peak would matter only in a CPU-bound frame. The worst frames in both
  runs are the port's step spikes (queued separately), not the step rate.
- **Stepping physics on the workers while the main thread renders the
  previous poses (double-buffered transforms) is not worth queueing now.**
  The GPU already overlaps the CPU: a frame takes max(CPU, GPU). The CPU
  part is ~3.4-3.7 ms (physics 1.3-1.8 of it), well under the GPU's 6-9 ms
  even in the small window. Taking physics off the main thread would not
  move the frame. Revisit when the renderer's GPU frame drops below the
  CPU frame, or if the step spikes stay after their fix: async stepping
  would hide a spike behind the previous poses.

### 10g. The low end: no catch-up spiral, and WASM (2026-10-06)

**The fixed-step accumulator.** `consumeFixedSteps` (both tracks:
`lib/physics/ecs/Physics.rae`, `lib/box3d/ecs/Physics.rae`) now takes at most
**2** fixed steps per frame (`maxStepsPerFrame`, was 4): one catch-up step.
After them, whole steps still owed are dropped and the fraction of a step is
kept, since it is the blend PoseBlend draws at. The old rule clipped the
accumulator to the cap before stepping. At a cap of 2 that would also have
thrown away the ordinary remainder of an uneven frame (10 ms left over plus a
33 ms frame), losing time on a machine that was not behind. Fixtures 988 and
999 drive exactly such uneven frames and are unchanged under the new rule,
since no frame of theirs owes more than 2 steps.

Fixture 1009 models the loop (a 9 ms render plus each step's cost, with a
frame's length the next frame's time) through `consumeFixedSteps` itself:

| case | cap 4 (before) | cap 2 (now) |
|---|---|---|
| 3 ms steps | 12 ms frames, real time | the same |
| 12 ms steps | 33 ms worst, real time | 33 ms worst, real time |
| 20 ms steps (slower than the 16.7 ms they simulate) | **89 ms** frames of 4 steps, 0.75x real time | **49 ms** frames of 2 steps, 0.68x |
| a 100 ms hitch, then 3 ms steps | 21 ms catch-up frame (4 steps) | 15 ms (2 steps) |

A step slower than the time it simulates is the spiral: each frame owes more
than it pays. With a cap of 4 it settles at four-step frames, the playground's
40-80 ms frames (§10). The cap of 2 bounds the worst frame at render plus two
steps and lets the simulation run slower instead.

**WASM.** The port's step in WebAssembly, single-threaded, as a browser
without WASM threads runs it. `benchmarks/physics`'s scenes were emitted with
the release profile (the Float4 SIMD lowering) and compiled with wasi-sdk
(`-O2 -msimd128`, wasm32-wasip1). They ran in Node 26's WASI against a native
binary built from the same C, 1 worker, best of 2, ms per step:

| scene | native | WASM (Node) | WASM / native |
|---|---|---|---|
| large_pyramid (5 050 boxes) | 12.80 | 16.42 | 1.28x |
| many_pyramids | 24.91 | 31.56 | 1.27x |
| joint_grid | 15.12 | 18.83 | 1.25x |
| convex_pile | 16.75 | 25.70 | 1.53x |
| rain | 6.16 | 7.73 | 1.25x |

Every checksum is identical to native: the port is bit-exact in WASM too.
Single-threaded, a 5 000-box pyramid takes ~16 ms per 60 Hz step, so it just
keeps real time with no room for the frame. With the cap of 2, a browser
falls back to running the simulation slower, not to a spiral. WASM threads
(the wasip1-threads build `compiler/tools/wasm_build.sh` already has,
WASM_THREADS=1; backlog #245 for the browser) would give back the §10e
scaling.

**The playground in the browser** (2026-10-06, fixed the same day). `rae build
--target wasm` had failed for every WebGPU app, on wgpu-native's
`webgpu/wgpu.h`, which the generated bindings declare as their cheader.
Five fixes got it running:
- `compiler/runtime/web/webgpu/wgpu.h`, on emcc's include path only, maps
  that header to the browser's standard `webgpu.h`. It also gives
  `wgpuQueueSubmitForIndex` a browser version: a submit plus an increasing
  index, which is all GpuLifetime's completion tracking needs.
- The bindings' enum constants come from the header natively and from their
  Rae literals in the browser. The 493 constants the two headers share have
  the same values; Dawn lacks wgpu-native's 131 extension constants.
- Browser builds get an 8 MB stack (`-sSTACK_SIZE`, as a native process
  has); Emscripten's 64 KB default overflowed in the physics step. The
  `RAE_WGPU_REPORT` diagnostic is native-only. `lib/physicsScenes/scenes`
  is preloaded with the other `lib` scene folders.
- Two shaders broke Dawn's stricter WGSL uniformity rule. The deferred
  composite's FXAA samples its one-mip target with `textureSampleLevel`,
  and the glyph shader takes its clip coverage (`fwidth`) before its
  branch. Native output is unchanged.
- The web shell passes URL query parameters named `RAE_*` as environment
  variables, so `index.html?RAE_PLAYGROUND_PROFILE=300` profiles in the
  browser.

`compiler/tools/wasm_webgpu_smoke.sh` passes again and now runs as a case of
every full suite run (~50 s; SKIP without emcc).

Port playground in headless Chrome 154 (WebGPU on Metal), single-threaded,
release, 1280 x 800 window, ms per frame:

| scene | whole frame | physics | render CPU | the rest |
|---|---|---|---|---|
| pyramid, 5 000 boxes, autofire | 44.1 (~23 fps) | 31.2 (1.55 steps a frame: ~20 ms per step) | ~1.6 | 10.6 waiting for the browser's frame |
| pyramid, 1 000 boxes, autofire | 16.67 (60 fps, vsync) | 0.12 | | 15.4 waiting for the browser's frame |

On the web the profile's "input, camera" lap holds the wait for the browser's
next animation frame (the Asyncify yield in the poll). The browser's step was
~20 ms at 5 000 boxes against 16.4 in Node's WASI. That turned out to be the
browser build's missing `-msimd128`, not Asyncify (docs/web-frame-loop.md
§2). With SIMD on, the step is ~12.7 ms and the frame 35 ms (was 44-52);
Asyncify itself costs ~4% of the step. At 5 000 boxes the browser cannot keep real time on one
thread; with the cap of 2 it runs the simulation slower, at 1.55 steps a
frame. At 1 000 boxes it is smooth at 60 fps. WASM threads (backlog #245)
are what would bring 5 000 back.

### 10h. The step's spikes, the spawn hitch and a zero-allocation step (2026-10-06)

**The spikes.** Reproduced with the windowed playground (pyramid, 5 000
boxes, autofire, `RAE_PRESENT_MODE=immediate`, 1 200 profiled frames), with
and without 6 CPU burners at the default QoS class (`perl -e '1 while 1'`).
"Spikes" counts frames whose physics step took over 8 ms:

| run | spikes | step avg / worst (ms) |
|---|---|---|
| port, 8 workers at DEFAULT QoS (before), 6 burners | 184 | 2.84 / 29.5 |
| Box3D C (its own scheduler), 6 burners | 120 | 3.02 / 52.0 |
| port, 8 workers at the launcher's QoS, 6 burners | 5, 5 | 1.57-1.61 / 12.5-13.3 |
| port, 7 workers at the launcher's QoS, 6 burners | 2 | 1.49 / 13.3 |
| port, 8 workers at the launcher's QoS, no burners | 2, 4 | 1.27-1.36 / 11.6-12.0 |
| port, 7 workers at the launcher's QoS, no burners | 0, 0 | 1.17-1.22 / 4.9-6.9 |

- **The cause was preemption, and it was not the port's ~100 barriers.**
  Box3D, which runs its solver as one task, spiked just as badly under the
  same load: a stage join waits for every claimed chunk, and a worker
  preempted mid-chunk holds one for a whole time slice (~10 ms on macOS).
  Both pools already steal unclaimed work, so a worker that is merely late
  costs nothing.
- **The pool's threads ran below their launcher.** macOS starts a new
  pthread at QoS DEFAULT (0x15), while an app's main thread, the one waiting
  at every join, is USER_INTERACTIVE (0x21). Any default-class work on the
  machine then competed with the workers on equal terms. The pool now creates
  its workers at the launching thread's class (`pthread_attr_set_qos_class_np`
  in `runtime_threads.c`).
- **The pool took every performance core.** At 8 workers on 8 performance
  cores, the program's own render thread, the GPU driver and the window
  server still preempt a worker now and then. The default is now
  **performance cores minus one** (7 here). Headless, at 5 000 and 10 000
  boxes, 7 and 8 workers are level within noise (2.7-3.2 against 3.0-3.1 ms;
  5.8-6.4 against 6.0-6.2 ms), and in the window 7 is faster. `RAE_WORKERS`
  still sets any count.
- Box3D's built-in scheduler (track B) creates its threads at DEFAULT and
  Rae cannot reach them without patching upstream, so track B keeps its
  spikes under load; it uses the same 7 workers (`Parallel.workerCount()`).

**The 1 000-body spawn.** `RAE_PLAYGROUND_SPAWN=1000` (new, headless):
spawning the pyramid at once into a fresh world (sleeping off), then its
first steps against a steady one:

| | spawn | first three steps | steady step (median) |
|---|---|---|---|
| port | 1.75-1.96 ms | 2.8, 0.9-1.1, 0.8 ms | 0.73-0.81 ms |
| Box3D C | 1.22 ms | 3.6, 0.6, 0.7 ms | 0.72 ms |

There is no hitch to remove: spawn plus the first step is ~5 ms on both
tracks, inside one frame. The first step costs ~4x a steady one because a
thousand new proxies find their pairs at once. The port allocates on the
way (19 allocations per box: 13 for its box hull's Lists, then 3 per island
and per contact the first time a slot is used). Contacts and islands reuse
their slot's Lists when recycled, so this is pool growth that stops once
the pools are warm, not per-step traffic.

**The steady step allocates nothing.** `Sys.allocationCount()` (new: the
runtime's always-on count of String and List/Buffer allocations) across 120
steady steps: the port allocated **720** (6 a step), all in
`physicsEventSystem`. `getContactEvents`/`getSensorEvents` return copies of
the world's five event Lists, and the system took those copies every step.
It now reads the lists in place, and the count is **0**. Track B also counts
0, but the counter only sees Rae's allocations, not Box3D's own C mallocs.
