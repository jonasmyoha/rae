# Parallelism first — rough plan

**Status:** proposal (2026-09-13). Companion to `docs/concurrency-model.md`
(the language model — authoritative) and `docs/physics-design.md` (Box3D,
which does not depend on any of this). The question this answers: should Rae
get real parallelism — thread pool, `parallelLoop`, parallel ECS systems —
*before* the physics engine, driven by the small examples, so the games adopt
the idiom from day one instead of being retrofitted?

## 0. Verdict

**Yes, with two corrections.**

1. Parallel-by-construction is cheaper than retrofit — but only if the
   *scheduling model* is fixed early. The expensive retrofit in a game is not
   sprinkling `parallelLoop`; it is discovering late which systems may run
   concurrently. Rae is unusually well placed: parameter modes already say
   what each system reads and writes. What is missing is the **schedule
   declaring it**. That is the thing to build first, on small examples.
2. Pick workloads that actually have CPU work. The metaballs example
   evaluates its field on the GPU (WGSL SDF); its per-frame CPU cost is UI
   and cluster animation. It is a fine *conformance* example (parallelism
   must cost nothing when there is nothing to do) but a useless benchmark.
   Real CPU work today: crowd pose palettes and terrain generation in the
   walker example, and per-soldier systems (spatial-hash separation, AI,
   combat, arrows) in one downstream game prototype.

Physics is not blocked by this: Box3D threads itself (`workerCount`) and is
deterministic across worker counts. The only ordering benefit is real but
modest — with declared read/write sets in the schedule, the physics step is
"one node that writes transforms" from day one.

## 1. What exists (verified against the tree)

- `spawn f(args)` → `Task(T)`, `task.get()`, join-on-drop, `taskScope { }`
  (desugars to a run-once scope), `List(Task(T))` works (fixture 646),
  `Channel(T)` (fixture 541). **Compiled backend runs real OS threads** for
  value / `own` / `copy` heap arguments (deep-copied at the spawn site; ASan
  clean); `mod` and `view`-of-aggregate arguments fall back to sequential
  (`c_spawn_threadable`, `compiler/src/c_backend.c`).
- **One OS thread per spawn** (`runtime_threads.c`: `RaeTask` + `pthread_create`).
  No pool. Fine for a handful of long workers, wrong for per-frame jobs.
- `parallelLoop` parses (same grammar as `loop`, `is_parallel` flag) and
  **compiles as a sequential loop**.
- No `detach`. No disjointness checking.
- Runtime globals: the string temp pool and RNG are `__thread`; the memory
  counters `g_mem_*` are plain globals (0 atomics) but only counted under
  `RAE_MEM_STATS=1` — a test-mode race, still an audit item.
- `lib/ecs/Schedule`: entries are a *name* + a caller-supplied read
  generation for `shouldRun`; **read/write sets are not declared**.
- `lib/Profile.rae`: `zoneBegin`/`zoneEnd`/`counter`, timed captures to a
  file — the measurement tool the plan starts with.
- WASM: `WASM_THREADS=1` builds `wasm32-wasip1-threads` with `-pthread`;
  browsers need cross-origin isolation for SharedArrayBuffer.
- `docs/concurrency-model.md` §5 ("current state") predates the C-backend
  work its own header lists — refresh it as part of task 1.

## 2. The plan, in order

### Phase 0 — measure (one small task)

Add `Profile` zones around every system in the walker example and the two
downstream prototypes; capture a few seconds; rank CPU cost per system.
Everything below is prioritised by that table, not by intuition.

### Phase 1 — runtime foundation (compiler + runtime)

1. **Thread-safety audit** of the runtime under real threads: `g_mem_*` →
   atomics or per-thread counters merged at exit; any remaining process
   globals (registries, interned pools). Gate: the four spawn fixtures pass
   under TSan.
2. **Worker pool** in `runtime_threads.c`: `N = performance-core count`
   workers, a work queue, `rae_job_submit` / `rae_job_wait`. `spawn` keeps
   its thread-per-task semantics (long-lived workers, channels); pool jobs
   are what `parallelLoop` and `taskScope` children run on.
3. **Real `parallelLoop`** (index form first): codegen splits the range into
   fixed-order chunks submitted to the pool and joins. Body rules for the
   first slice: `view` anything; `mod` only through the loop index (`xs[i]`,
   `queryModAt(table, i)`, `table.data[i]`); anything else is a compile error
   with a diagnostic naming the offending access. Reductions are written as
   per-chunk partials into a pre-sized list merged in order (the 511
   raytracer idiom), so the result is **independent of worker count**.
4. **Determinism as a language guarantee**: `RAE_WORKERS=1` and `=8` must
   print identical output for every parallel fixture — that is the property
   lockstep multiplayer needs, and it is cheap to hold if chunk order and
   reduction order are fixed by construction.
5. **`taskScope` for real**: join-before-exit, non-escape of child tasks,
   and — because the scope joins before the parent continues — children may
   borrow `view` aggregates from the parent (model §2b). Then `detach` for
   genuine fire-and-forget workers.
6. Extend `c_spawn_threadable`: POD struct by value (trivial), `view`
   aggregates inside `taskScope`.
7. **WASM**: `parallelLoop` on the threads build; a pool of size 1 when
   SharedArrayBuffer is unavailable; the example gate runs both.

### Phase 2 — the schedule declares reads/writes (lib/ecs, tiny compiler help)

1. `addSystem(schedule, name, reads: [tableIds], writes: [tableIds])`.
   The runner groups consecutive entries into **stages**: systems whose
   write sets are disjoint from each other's read+write sets run
   concurrently under one `taskScope`; a conflict starts a new stage.
   Stage assignment is a pure function of the declarations — deterministic
   and printable (`rae` can dump the frame DAG).
2. Component-table data parallelism: `parallelLoop` over a table's dense
   range with `queryModAt`; `query2`/`query3` joins split by index range.
3. **Main-thread-only** for GPU/platform calls (WebGPU, SDL, audio): a
   documented rule now, a compiler check later (the bindings are already
   identifiable by module path, as `uses_webgpu` detection shows).

### Phase 3 — examples as the conformance suite (smallest first)

1. Fixtures: the 511 raytracer rewritten as `parallelLoop` over rows; a
   worker-count determinism fixture; an ordered-reduction fixture; a
   rejected-aliasing diagnostic fixture.
2. **Walker example**: crowd pose palettes per character in a
   `parallelLoop`; `makeFbmTerrain` rows in parallel at startup;
   `transformSystem` / `prevTransformSystem` as parallel loops over dense
   arrays; input / camera / animation / grass as schedule stages with declared
   sets. Profile before/after — this is the benchmark.
3. **Metaballs example**: declared sets on its schedule; cluster animation as
   a parallel query loop; verify an idle frame does not spin the pool and
   the render loop's "is animating" flag is untouched.
4. Procgen / water examples: startup generation chunked in parallel.

### Phase 4 — the game prototypes

- **Downstream game A** (soldiers): `soldierSeparationStep` per spatial-hash
  cell, AI / combat / arrow steps as parallel systems with declared sets;
  the net tick stays serial. The Phase 1.4 guarantee is what makes this safe
  for lockstep — without it, parallel AI desyncs peers.
- **Downstream game B** (track): little CPU load yet; adopt the declared
  schedule now while it is small, crowd animation palettes in parallel.
  When Box3D lands, `physicsStepSystem` is one declared node (writes
  transforms; Box3D `workerCount` threads internally).

### Phase 5 — physics

`docs/physics-design.md`, unchanged, now written against declared sets from
the start. Later: Box3D's external-scheduler hook onto the Rae pool once
Rae-function-to-C callbacks exist, so the two never oversubscribe cores.

## 3. Risks

- **Disjointness soundness** is the make-or-break item (the model's own #1
  risk). The first slice is deliberately syntactic and conservative; reject
  more, not less.
- **Hot reload with live jobs** (`rae watch`): quiesce the pool at a reload
  boundary; forbid reload mid-stage.
- **Oversubscription** once Box3D workers and the Rae pool coexist — solved
  by the external-scheduler hook, not before.
- **Efficiency cores** (Apple): size the pool by performance cores, as
  Box3D's docs also advise.
- **WASM threads** need COOP/COEP headers; keep the single-thread build the
  default artefact.

## 4. Task sizing (proposed queue items, next free id at time of writing #939)

1. Phase 0 profiling pass (small). 2. Runtime audit + TSan gate (small).
3. Worker pool (medium). 4. `parallelLoop` codegen + diagnostics (large).
5. Determinism guarantee + fixtures (small). 6. `taskScope`/`detach` (medium).
7. `c_spawn_threadable` extensions (small). 8. WASM threads path (medium).
9. Schedule read/write sets + stages + DAG dump (medium). 10. Table
`parallelLoop` + split joins (small). 11–14. Walker / metaballs / procgen /
fixtures (per-example, small each). 15–16. The two prototypes (per-system).
17. Refresh `concurrency-model.md` §5 (tiny, fold into task 2).

## 5. The UI example (106): concurrency versus parallelism, and one architecture

The mobile-UI example is the counter-example that sharpens the plan: it has
almost no data-parallel CPU work, and a great deal of **waiting**.

What it does today (verified):

- **Waiting, hand-built in C because Rae had no `spawn` yet:**
  `lib/sys/Spotify.rae` exposes `startPoller` (a runtime `pthread` that runs
  `osascript` every N ms so the UI thread never forks), `fetchArtworkAsync` +
  `fetchArtworkStatus` (one runtime `pthread` per curl download, observed by
  polling a status code), and a blocking `fetchArtwork` whose use the app had
  to ration: `refetchHistoryArtChunk` processes `budget` entries per frame
  "so the UI does not beach-ball". That is an async/await problem solved with
  a C thread pool and a per-frame poll.
- **Computing:** per-frame systems are dirty-driven (`shouldRun` on table
  generations; layout/transform skip when nothing changed) and the idle loop
  sleeps in `waitEvents`. Boot decodes 15 PNGs (stb_image, CPU) and parses
  scenes/JSON/SDF font tables synchronously — the only real parallel
  candidate, and only at startup.
- **No waker:** `waitEvents` is `SDL_WaitEventTimeout`; the example's
  `uiInstallWindowCloseWaker` is an empty stub. A background completion cannot
  wake an idle loop; it is noticed on the next timeout tick.

So: **106 benefits from `spawn`/`Channel` (concurrency), barely from
`parallelLoop` (parallelism), and needs one new primitive — a waker.**

Concretely, with the language as it is:

1. Replace the C poller + curl job table with Rae workers: `spawn
   pollSpotify(chan)` and `spawn fetchArtwork(url, outPath, chan)` posting
   results on a `Channel(ArtworkResult)`; the frame loop drains the channel
   in a `historyArtSystem` and registers textures. The chunk/budget dance
   disappears; `refetchHistoryArtChunk` becomes "drain what arrived". The C
   left behind is the genuine ABI (`curl` / `osascript` invocation), not the
   scheduling — the same boundary as the renderer.
2. Add **`wake()`** to the event loop (`ui/EventLoop`): a thread-safe call
   that posts a user event so `waitEvents` returns immediately. A worker calls
   it after sending on the channel. Without it, async completions render one
   timeout late; with it the idle loop stays at 0 % and still reacts.
3. Boot: `taskScope { }` around the 15 cover decodes and the scene/JSON
   parses — a one-line parallel startup once `taskScope` children may borrow
   `view` data (Phase 1.5). Not a per-frame concern.
4. `detach` for the poller (a worker with no result to join), once it exists;
   until then it is joined at shutdown, which is fine.

**On "games and other programs should share one architecture" — agreed, and
this is the evidence.** Both kinds of program are the same shape: an ECS world
stepped by a schedule, with two kinds of concurrency at the edges of each
frame — *waiting* (`spawn` + `Channel`, results drained by a system) and
*computing* (`parallelLoop`, systems in parallel stages). A game leans on the
second (crowds, physics, AI) and a UI app on the first (network, subprocesses,
disk), but every game also waits (asset streaming, netcode, save/load) and
every app also computes (image decode, text shaping, search). The same
schedule, the same modes, the same two primitives; only the mix differs. What
must *not* differ is the loop policy: the wait-based idle loop
(`nextWaitTimeoutSec` + `wake()`) for apps, the busy loop for games, and the
hybrid the UI-render-loop postmortem already prescribes — the concurrency
primitives plug into whichever loop the program owns.

Task additions: 18. `EventLoop.wake()` (runtime user event + Rae API, small).
19. Port 106's Spotify/artwork path to `spawn` + `Channel` (medium; deletes
C scheduling code). 20. `taskScope` boot decode in 106 (small, after 1.5).
