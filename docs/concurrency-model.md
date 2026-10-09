# Rae Concurrency & Threading — design

Status: **implemented on the Compiled (C) backend — the only target** (the
Live/bytecode VM this document was first written against was removed in #957).
Refreshed against the tree 2026-09-14 (#953). §5 is the current state; §1–§4
are the model, unchanged; §6–§9 are annotated with what landed and what is
still open. Fixtures: `013_spawn_calls`, `360_spawn_concurrency`,
`511_spawn_raytracer_bands`, `512_spawn_string_workers`,
`513_spawn_own_list_copy`, `541_channel_worker`, `646_list_of_tasks`; the
first production consumer is 106's background I/O (#950, `docs/parallelism-
first-plan.md` §5).

In one paragraph: `spawn f(args)` returns a joinable `Task(T)`; `task.get()`
joins exactly once and yields the result (`task.isDone()` / `task.tryGet()`
ask without blocking); a `Task` local that goes out of
scope without `get()` is joined on drop (`rae_task_drop`); `taskScope { }`
is a run-once block whose tasks therefore join at its exit; `Channel(T)` is
an MPSC, Int-payload, non-blocking channel for long-running workers;
`parallelLoop` parses and runs sequentially. Real OS threads are used when
every argument of the spawned function is capturable by value or by a
private deep copy (scalars, enums, `own`/`copy`/plain `String`, heap
aggregates, POD structs); pointer-into-parent shapes (`mod`, `view` of
anything but a scalar) fall back to running the call synchronously into a
completed task — same observable result, no parallelism.

The design came out of a roundtable (Chattie / Clo / Gem) plus a final
maintainer pass. It deliberately diverges from the roundtable on two points:
**no implicit task→value coercion**, and **no early green-fiber VM scheduler**
(moot now that there is no VM).

---

## 1. Philosophy

Rae inverts the usual `async`/`await` convention, because the usual one is
backwards for a language that values explicitness:

- **Normal calls are synchronous and waited** — the default, unmarked.
- **`spawn` is the only marker for starting concurrent work** — you opt *into*
  concurrency, not out of it.
- **Synchronization is explicit and visible** — `task.get()`, never an implicit
  blocking coercion hidden inside an ordinary assignment or argument.

There is **no `await` keyword and no function colouring.** A function is not
"async"; only the *call site* decides (via `spawn`) whether it runs concurrently.

Concurrency safety is **not** a new subsystem of locks. It rides on Rae's
existing parameter modes — `own` / `copy` / `view` / `mod` — which already
encode move / duplicate / shared-read / exclusive-write. That is the same
information Rust derives from `Send`/`Sync`, except Rae already computes it for
every parameter. The compiler proves race-freedom **at the `spawn` and
`parallelLoop` boundaries**, so we avoid a lock-everything runtime. This is the
spine of the design and its biggest single risk (see *Risks*).

One execution backend, one language semantics: **Compiled (C)** — genuine
parallelism on real OS threads. (The design originally also named a Live/VM
backend that would run tasks sequentially with the same ownership checks; it
was removed in #957, see §6.)

---

## 2. The two kinds of concurrency (they have different rules)

The key insight: a single capture rule cannot serve both independent tasks and
scoped parallel work. Split them.

### 2a. Independent tasks — **move/copy only**

```rae
let task = spawn loadImage(path: own path)
let image = task.get()
```

An independent task may **outlive the statement that started it**, so it must
not hold borrows into the caller's stack or mutable state. Captures are
restricted to:

- `own T` — the value is **moved** into the task; the caller can no longer use it.
- `copy T` — the value is **deep-copied** into the task (only for copyable `T`).

`view T` / `mod T` captures into an independent task are a **compile error.**
This is not a stylistic choice — it matches what the runtime physically does
today (the spawn arg transfer is a shallow pointer-move; see *Current state*),
so a borrowed capture would alias the parent's heap across threads.

### 2b. Scoped parallel work — **may borrow**

```rae
parallelLoop index in 0 ..< positions.length {
  positions[index] = update(
    position: positions[index]
    velocity: view velocities[index]
  )
}
```

A `parallelLoop` (and `taskScope`) **joins before it returns.** Because the join
is a hard barrier, the borrow provably outlives every child, so scoped work
**may** borrow `view` data and **provably-disjoint** `mod` regions from the
enclosing scope. This is what enables the "many workers over component tables,
then join" pattern the language is meant to support.

> Ownership mode decides *what is safe to share*; **structured-vs-detached
> decides whether borrowing is allowed at all.** No trait system required.

---

## 3. Surface API

### `spawn` → `Task(T)`

```rae
let task: Task(Image) = spawn loadImage(path: own path)
let image: Image = task.get()
```

- A bare `spawn` evaluates the call on a new task and returns a `Task(T)`.
- **Dropping a running `Task(T)` joins it** — it does **not** silently detach.
  Leaking a thread should never be the accidental default.
- Explicit detachment is a later, deliberately-conspicuous operation:
  ```rae
  detach spawn telemetryWorker(channel: own channel)
  ```
  Detachment is uncommon and visibly dangerous; it is **not** in the first
  milestones.

### `Task(T).get()` — explicit synchronization

```rae
let result = task.get()
```

`get()` **waits for completion and retrieves the result** (and re-raises a task
failure — see *Errors*). We use `get()` rather than `join()` because it
communicates both the wait *and* the value; `join()` traditionally implies
wait-only.

**There is no implicit `Task(T)` → `T` coercion.** Writing `let r: Int = task`
is rejected. Hiding a potentially long blocking wait (and potential deadlock)
inside an ordinary assignment or function argument would defeat the whole point
of making concurrency explicit. The desired inversion is preserved without it:

- ordinary call → synchronous,
- `spawn` → concurrent,
- `task.get()` → explicit synchronization point.

### `Task(T).isDone()` / `tryGet()` — asking without blocking

```rae
var task: Task(Image) = spawn decode(path: path)
loop not task.isDone() {
  drawFrame()                       # the frame goes on while the task runs
}
if let image: Image = task.tryGet() {
  show(image: image)
}
```

- `isDone() ret Bool` — has the task finished? Never blocks.
- `tryGet() ret opt T` — the result once the task has finished, `none` while
  it runs. It hands the result out **once**: the task is joined (instantly —
  it has finished) and the value is moved out exactly as `get()` would; a
  later `tryGet()` is `none`. A heap result (a `String`, a `List`) is then
  owned by the caller and dropped by it, once.
- `tryGet()` needs a task with a result (`Task(Void)` is rejected: poll
  `isDone()`, then `get()`). A task never asked at all is still joined when
  it goes out of scope.
- This is how a frame-driven server picks up `spawn`ed slow work
  (docs/server-benchmarks-design.md §1.3, gap T1).

### `taskScope { … }` — structured concurrency

```rae
taskScope {
  let art = spawn loadArt(path: artPath)
  let music = spawn loadMusic(path: musicPath)

  useAssets(
    art: art.get()
    music: music.get()
  )
}
```

At scope exit every still-running child is joined (or cancelled, per the
cancellation rules below). Children cannot outlive the scope unless their
ownership/lifetime model explicitly permits it. `taskScope` is the **preferred
user-facing form** for a fixed set of concurrent operations.

Internally `taskScope` may be backed by a `TaskGroup`, but most users should not
need to name that type. A `TaskGroup` **may** still be exposed as a library type
for *dynamic* task collections (spawning a variable number of tasks in a loop
and joining them), but it is not the headline API.

### `parallelLoop` — data parallelism

Rae has one looping concept, so the data-parallel construct is `parallelLoop`,
**not** `parallelFor` (that name leaks C-family terminology). The exact surface
syntax must be co-designed with Rae's eventual range/collection-loop syntax;
provisional forms under consideration:

```rae
# index form (most likely first, fits ECS dense arrays)
parallelLoop index in 0 ..< positions.length {
  positions[index].x += velocities[index].x
}
```

A library/closure-shaped form (`parallelLoop(count:, body:)`) is possible but
depends on Rae gaining function values/closures, so it is deferred.

Semantics are fixed regardless of final syntax:

- It **always joins before returning** (it is scoped work, §2b).
- Each iteration may read shared `view` data.
- Mutable access must be **provably disjoint** (one iteration ↔ one index/region;
  no overlap). The compiler must verify this, or reject the loop.
- **Structural table operations are forbidden inside it** — no component
  add/remove, no list growth, no sparse-map mutation. Those stay sequential
  (see ECS rules).

### `Channel(T)` — long-running worker communication

```rae
channel.send(value: own message)
let message = channel.recv()
let maybe: opt Message = channel.tryRecv()
```

- `send(value: own T)` — the value **leaves** the sender (move).
- `recv() ret T` — blocks until a value is available.
- `tryRecv() ret opt T` — non-blocking; `opt T` matches Rae terminology (not
  `Option(T)`).

Channels are the right tool for: network workers, file/asset loading, audio
command queues, render-command / frame handoff, and any long-running service
thread. A long-running worker owns its state and talks to the rest of the
program **only** through channels — no shared mutable state, no locks.

### Shared mutable state — start minimal

Do **not** lead with a general `Shared(T)` mutex cell; it becomes the escape
hatch everyone reaches for and erodes the ownership model. The v1 toolkit is:

- `Channel(T)` (move-based message passing),
- **Atomics** for a small set of primitive types (`Atomic(Int)`, `Atomic(Bool)` —
  e.g. a worker "should-stop" flag, a counter),
- compiler-verified **disjoint** mutation inside `parallelLoop`.

A mutex-protected `Shared(T)` is added **only** after real use cases prove it
necessary.

### Platform-bound APIs are main-thread only

Rendering and window/event APIs (raylib `beginDrawing`/`endDrawing`, GLFW event
pump, GL calls) **must run on the main thread.** This is a documented hard rule;
the render loop stays on the main thread and receives frame data from workers
via a `Channel`. (Unanimous in the roundtable.)

---

## 4. ECS / component-table rules

The UI ECS (`lib/ui/ecs.rae`) stores each component type in a `ComponentTable`
with dense arrays plus an O(1) sparse map. Concurrency rules:

- **Parallel reads** of a table are fine.
- **Parallel writes** are allowed only over **disjoint dense index ranges** via
  `parallelLoop` — each worker owns a non-overlapping window, so no aliasing,
  no locks.
- **Structural mutation is single-threaded only**: `componentSet` / `componentRemove`
  touch the sparse map, bump the `generation` counter, and may reallocate the
  dense arrays. These must never run concurrently with any other access to the
  same table. A `parallelLoop` body may mutate component *values* in place but
  must not add/remove entities or grow the table.

---

## 5. Current state (what exists today, 2026-09-14)

Everything below is the Compiled (C) backend; there is no other engine.

**Front end.** `spawn` is a unary expression (`AST_UNARY_SPAWN`) typed
`Task(T)` for a callee returning `T`; `Task(T)` is a builtin generic
(`TYPE_TASK`) with three methods: `get(): T`, `isDone(): Bool` and
`tryGet(): opt T` (2026-10-08). `taskScope { … }` parses to a
run-once `if` block flagged `is_task_scope` (`parser.c`); `parallelLoop`
parses to a `loop` flagged `is_parallel`. `taskScope` gets its semantics from
join-on-drop; `parallelLoop` runs on the worker pool (below, 2026-10-03).

**Tasks on the scheduler (2026-10-09, docs/lightweight-spawn-design.md
§12).** A spawn whose task waits only on other tasks (`get`, a Task dropped at
the end of its scope, a `taskScope`) and calls no C beyond the runtime's own
runs on the worker pool instead of a thread: the C backend asks the may-wait
analysis per spawn (`may_wait_spawn_on_pool`), and the spawn site calls
`lib/core/Scheduler.rae`'s `schedulerSubmit`. The same pool runs
`parallelLoop`. A worker waiting on a task runs other tasks meanwhile; a
thread off the pool (`main`) that waits on a task nobody has started runs it
itself, and otherwise blocks. Every other spawn (one that sleeps, waits on a
socket or calls other C) is still a thread of its own, as below, until its
wait can suspend (S4) or its C call is marked `blocking` (S5). Since 0.1.252
(§13 of the design) a spawned function that may wait gets a resumable twin:
its `get()` and its sleeps suspend the task instead of holding a worker, so
sleeping spawns run on the pool as well.
`RAE_SPAWN_THREADS=1` at build time keeps every spawn a thread.

**Runtime (`compiler/runtime/runtime_threads.c`, `rae_runtime.h`).**
`Task(T)` lowers to `RaeTask*`: `{ pthread_t thread; void* result; _Atomic
int done; int joined; int taken; int on_pool; thunk; args }`, one pthread
per spawn, or a scheduler task (`on_pool`) run by a pool worker. The worker stores
its result, then `done` with release order; `isDone()` is one acquire load
(`rae_task_is_done`) and `tryGet()` lowers in `c_expr.c` to `rae_task_claim`
(done and not yet taken: mark taken), then the same join-and-read as `get()`,
filling the `rae_opt_<T>` struct (discovery registers `opt T`, which the
program may never spell). `taken` is touched only on the owner's thread.
`rae_task_await` joins once (guarded) and hands back the result buffer, which
the `get()` site casts to `T`; `rae_task_drop` joins if needed and frees. A
`view`/`mod Task(T)` parameter is a `RaeTask**` (a reference like any other). There is **no** running/failed
status and **no** failure propagation yet (§8 stays open). A `Task` inside a
struct or a `List(Task(T))` (fixture 646) is not cascade-dropped — the owner
joins it explicitly (see `ArtworkFetch.rae` in 106 for the pattern: copy the
handle out with `copyAt`, `get()`, let the copy drop, remove the slot).

**Spawn codegen (`c_backend.c` `c_spawn_threadable`, `c_expr.c`).** For each
spawned function a per-function pthread thunk packs the arguments and
`pthread_create`s. The threadable shapes, per parameter:

- scalars (`Int`/`Float`/`Bool`/`Char`, any mode but `mod`) — by value;
- enums (`own`/`copy`/plain) — by value;
- `String` (`own`/`copy`/plain) — the spawn site hands the worker a private
  `rae_string_copy`, the parent keeps and drops its own;
- heap aggregates (`List`/`Map`, non-generic non-`c_struct` user structs that
  own heap): an aliasing lvalue is deep-copied at the spawn site, a fresh
  rvalue (call result / literal) moves;
- POD structs (no heap → no cascade drop), `own`/`copy`/plain — by value.

Anything else — `mod` of anything, `view` of a `String`/enum/aggregate,
generic-instance or `c_struct` args — takes the **sequential fallback**: the
call runs synchronously on the caller and the result is stored into an
already-completed task. Verified ASan/UBSan-clean for the threaded shapes.
The String interpolation temp pool is `__thread`, so concurrent workers do
not corrupt each other.

**`Channel(T)` (`lib/Channel.rae`, #271, #969).** A value-type wrapper over
an opaque runtime handle; MPSC; a fixed ring of `sizeof(T)`-sized slots
guarded by a mutex hidden in the runtime; `createChannel(T)`, `channelSend`
(`value: own T` — moved into the ring), `pending`, `receive` (moved out, the
caller owns it), `received` (monotonic drained count), `freeChannel` (owner,
after the producers joined; drains and drops anything still buffered). Any
value type is a payload: Int, an enum, a POD struct, a struct owning
Strings/Lists — the runtime only hands out slot indices under its lock, the
typed store/load is the per-T `rae_ext_rae_buf_set`/`buf_get` specialisation
List uses, so `Channel(Int)` is the same code with an 8-byte slot (fixture
848 is the struct-with-String proof, leak-checked). No blocking receive, no
select: a consumer polls `pending`, a worker that wants to sleep uses
`sleep(ms:)`. Real use: 106's poll worker posts ticks on a `Channel(Int)`,
its artwork workers post `ArtworkResult { serial, ok }` on a
`Channel(ArtworkResult)`, and each calls `ui/EventLoop.wake()` (#950) so the
UI's `waitEvents` returns at once.

**Not implemented:** `detach` (a fire-and-forget worker; today a persistent
worker takes a stop channel and is joined at teardown), the
guaranteed-concurrent mode of `parallelLoop` (with the atomics-and-stages
design, below), atomics, task failure status /
propagation, `taskScope` cancel-on-error / non-escape enforcement, and
threading the `mod`/`view`-aggregate shapes (they are correctly sequential).
The staged plan for the rest is `docs/parallelism-first-plan.md` (and, for
what the physics port needs, `docs/physics-performance-plan.md`).

**The runtime is thread-safe for what a worker can call (2026-10-02).** A
spawned worker allocates and frees Strings and Lists, logs, reads the clock and
files, and wakes the event loop, exactly like the main thread. The audit of
every mutable static in the core runtime files found, and fixed:

- the allocation counters — the always-on `rae_mem_alloc_total`, the
  `RAE_MEM_STATS` per-site String counters, the buffer and String-pool counters
  — are C11 `_Atomic`, bumped with a relaxed add (one instruction, no
  ordering: a counter must be exact, not ordered);
- the `RAE_MEM_STATS` pointer-to-site hash table, and the `RAE_DEBUG_BOUNDS`
  buffer table, are guarded by a mutex (`RAE_LOCK`, a no-op in a WASM build
  without threads);
- the clock's lazily cached Mach timebase is read per call instead; the tick
  counter is atomic;
- the file-notification thread could mark a watcher changed AFTER
  `fileNotifyClear`, from an event it had already taken from the kernel (found
  by the gate below, whose slowdown widened the window); each kqueue
  registration carried a generation, and a stale event was dropped. Since
  2026-10-08 (runtime audit row 10) each FileNotify owns its kqueue and the
  thread is only a waker: it marks a queue's atomic "pending" byte and wakes
  the loop; `fileNotifyClear` drains the queue itself, and a stale mark is at
  most one spurious "look now".

Already safe: the String temp pool and the random state are `__thread`; the
program arguments are written once before `main`; the file-notify and Spotify
state have their own locks. Window, GPU, audio and file-dialog calls remain
main-thread only (§3).

**The gate:** `compiler/tools/tsan-check.sh` (`make tsan`, and one pre-suite
case of every full suite run) builds every threaded fixture — 511, 512, 513,
541, 646, 848, 941 — and 106_mobile_ui with its Spotify poller running, with
`-fsanitize=thread`, runs them with `RAE_MEM_STATS=1`, and fails on any
ThreadSanitizer report. Against the runtime before this audit it reports 23
races in 512 and 58 in 848, all in the counters and the stats table.

### `parallelLoop` on the worker pool (2026-10-03)

**The form.** `parallelLoop var i: Int = start, i < end, ++i { body }` — the
counted loop (`i++` too). Any other shape (a condition loop, a collection or
query loop) is a compile error naming this form: the runtime needs the range
up front to cut it into chunks. `start` and `end` are evaluated once.

**What an iteration may write** (§2b, checked in sema,
`sema_check_parallel_loop`): its own locals, and the element of its own index
in a shared List — `xs.set(index: i, value: v)`, `if let x: mod T =>
xs.modAt(index: i)`, `xs[i] = v`. It may read anything in scope. The compiler
rejects every other write two iterations could make to one place: assigning a
shared variable or field, `++` on one, a call that takes a shared value as
`mod` or `own` (including growing a shared List with `add`), a `mod` alias of
a shared place, writing an element at any other index — and every way out of
the outlined body (`ret`, a `break`/`continue` of the parallel loop itself).
Fixture 945 shows each diagnostic. A nested `parallelLoop` follows the same
rule with ITS index: a List local to the outer iteration is shared by the
inner one (fixture 944).

**The lowering** (`c_stmt.c emit_parallel_loop`). The body is outlined into
a C function that runs iterations [first, end); the loop becomes one
`rae_parallel_for(start, end, function, captures)` call, `captures` being the
addresses of the enclosing locals the body names. The function re-declares
each captured local under its own name and C type, so the body's C is exactly
what it would be inline; it owns nothing, so nothing is dropped twice. Each
call has its own String-pool mark (the pool is per thread).

**The pool** (`runtime_threads.c`). Workers = performance cores minus one
(`hw.perflevel0.physicalcpu` on macOS, so 7 on the M1 Max's 8; efficiency
cores would pace every join), the launching thread being one of them, started
on the first parallelLoop, joined at exit. The core left over is for the
program's other threads (renderer, GPU driver, window server), which otherwise
preempt a worker mid-chunk and stall the join; the workers run at the
launching thread's QoS class (2026-10-06, docs/physics-performance-plan.md
§10h). `RAE_WORKERS=n` sets the count;
`RAE_WORKERS=1` runs every parallelLoop sequentially. One job at a time: the
range is cut into about four chunks per worker, claimed through one atomic
word that packs the job's generation with the next chunk index (a worker late
from the previous job cannot claim a chunk of the next). After a job the
workers spin for some tens of microseconds, then sleep on a condition
variable — an idle program costs nothing (measured: 0.0% CPU, 8 threads
asleep). A parallelLoop inside a parallel body, or one started while another
thread's job runs, runs inline on its caller.

**Determinism.** The results are those of the sequential loop for any worker
count: no iteration can see another's writes. Fixture 944 (a million-element
transform, per-chunk partial sums combined in chunk order, structs updated in
place, Strings built per element, a nested loop) prints the same bytes at 1,
2, 4 and 8 workers, leaks nothing, and runs clean under ThreadSanitizer (it is
on the gate's list).

**Measured** (`benchmarks/parallel_loop/run.sh`, M1 Max, load ~6 from other
work, release build):

| workers | launch, back to back | launch after 2 ms idle | compute kernel | speedup |
|---|---|---|---|---|
| 1 | 0.01 us | 0.65 us | 135 ms | 1.00x |
| 2 | 0.27 us | 4.7 us | 68 ms | 1.98x |
| 4 | 0.62 us | 7.8 us | 35 ms | 3.82x |
| 8 | 3.9 us | 15.9 us | 22.6 ms | 5.96x |

The compute kernel is 1 000 000 elements of 64 multiply-adds; its checksum is
identical at every worker count. A launch is microseconds, as the physics step
needs (~10 parallel-for launches outside the solver, `docs/physics-performance-
plan.md` §5).

**Per-worker scratch is chunk scratch (decided 2026-10-03).** Box3D gives
each worker its own bitsets, pair-key arrays and event lists and merges them
after the parallel section. Rae does the same with no new rule: the
parallelLoop index is a CHUNK, each chunk owns a contiguous slice of the
input and the scratch slot of its own index, and the slots are merged in
chunk order —

```rae
let chunks: Int = Parallel.workerCount() * 4
# scratch: List(PairBuffer), one pre-sized slot per chunk
parallelLoop var c: Int = 0, c < chunks, ++c {
  if let buffer: mod PairBuffer => scratch.modAt(index: c) {
    let start: Int = c * count / chunks
    let end: Int = (c + 1) * count / chunks
    loop var i: Int = start, i < end, ++i {
      # append what element i produces into buffer
    }
  }
}
# merge scratch[0 ..< chunks] in chunk order
```

Because each chunk covers a contiguous slice and the merge walks the chunks
in order, the merged result is the sequential loop's, byte for byte, for any
worker count and any chunk count — even for an order-dependent merge
(concatenating pair keys or events), so no sort-afterwards step is needed for
determinism; OR-merges and sums work the same. Memory stays at k ×
workerCount slots, and k = 4 keeps the pool's dynamic load balancing. Fixture
946 is this idiom: one order-sensitive sequence, merged at the chunk counts of
1, 2, 4 and 8 workers and on the pool, identical to the sequential sequence
every time (and at RAE_WORKERS=1/2/4/8, and under TSan).

A `Parallel.workerIndex()` with a second write rule for its slot was
considered and rejected: which iterations a worker runs depends on
scheduling, so its merge is deterministic only if the user makes it
order-independent — something the checker cannot verify — while
worker-count determinism is a guarantee by construction
(`docs/parallelism-first-plan.md` item 4). It would also be a write rule that
only exists for parallel code. If the slice arithmetic keeps recurring, a
library helper is welcome; a new checker rule is not.

**Staged work: a stage is a `parallelLoop` (decided 2026-10-03,
`docs/parallel-stages-design.md`).** No guaranteed-concurrent mode, no
atomics, no barrier construct. A solver stage is a `parallelLoop` and the
stage sequence an ordinary `loop`. The pool keeps its workers spinning across
a step's stages, so a barrier costs about what Box3D's own spin barrier does
(§9 of that document). `RAE_WORKERS=1` runs the stages in order, and nothing
can deadlock.

**`unsafe { }` in a parallel body** lifts the write rule for its statements.
It is for writes that are disjoint by an argument the compiler cannot check —
a graph colouring keeping a colour's constraints off each other's bodies. The
programmer vouches for it, and the runtime checks it when asked:
`RAE_PARALLEL_CHECK=1` (or `Parallel.checkWrites(enabled: true)`) stops the
program when two iterations of one `parallelLoop` write the same List element
inside `unsafe`, naming both (fixture 947). Until
then the solver is sequential and the parallelLoop rules above are the whole
rule set.

---

## 6. Execution engine

There is one engine: the Compiled (C) backend. The Live/bytecode VM that the
first revisions of this section described ran `spawn` synchronously and was
removed in #957 — with it went the "Live runs sequentially first" staging and
the Live↔Compiled observable-equivalence suite. What the design asked of the
compiled engine, and where it stands:

- Real OS threads over pthreads — **done** for the threadable shapes (§5),
  one thread per spawned task; a persistent worker pool runs `parallelLoop`
  (§5).
- `Task(T)` = heap struct with typed result slot — **done**; non-blocking
  `isDone()` / `tryGet()` over an atomic done flag — **done** (2026-10-08);
  a failed status and a condition variable — **not yet** (join is
  `pthread_join`).
- `Channel(T)` = mutex-guarded queue — **done** as MPSC over any value type
  (#969); the MPMC + condvar (blocking receive) form is **not yet**.
- Atomics via C11 `<stdatomic.h>` — **not yet**.
- `parallelLoop` = genuine parallel execution over disjoint shards — **done**
  (2026-10-03, §5): the worker pool, the counted form, the write rule checked
  at compile time; per-worker scratch is chunk scratch (§5). The concurrent
  mode comes with the atomics-and-stages design.

Scripting / hot-reload roles the VM once nominally filled are reassigned to
native-hosted WASM modules (`docs/raepack-v2-and-packages.md`); a WASM thread
path is item 8 of the parallelism plan.

---

## 7. Roadmap (staged, in order)

*Annotated 2026-09-14: steps 1–2 landed (as compiled-backend work once the VM
went), step 3 landed as "parses, runs sequentially", step 4 is partly landed
(threads + `Task` + `Channel`; the pool, atomics and real `parallelLoop` are
open), step 5 is moot. The live successor of this list is
`docs/parallelism-first-plan.md` §4.*

1. **Make the existing VM `spawn` joinable and result-returning.** (See the
   precise milestone below.)
2. **Make task ownership and cleanup sound** — join-on-drop, single-join,
   correct ownership transfer of the returned value, reject unsafe captures.
3. **Implement Live `parallelLoop` sequentially** — establish the construct and
   its disjointness checks with the simplest possible engine.
4. **Add C-backend task execution and real parallel loops** — thread pool,
   `Task` runtime, channels, atomics, real `parallelLoop`; the global runtime
   state was audited and made thread-safe first (2026-10-02, §5).
5. **Reconsider a VM task scheduler** only when Live needs real interleaving.

`Channel(T)`, `taskScope`, atomics, and `detach` slot in alongside steps 2–4 as
their dependencies land. A `rae run --target live` vs compiled **observable-
equivalence** test suite is mandatory once both backends run tasks.

### The first milestone (precise, independently testable)

Target program:

```rae
func compute() ret Int {
  ret 42
}

let task: Task(Int) = spawn compute()
let result: Int = task.get()
```

The `Task(Int)` must:

- **Own** the thread handle (no leak; replaces the current discard).
- **Own** a typed result slot.
- Record **running / completed / failed** status.
- **Join exactly once.**
- Transfer **ownership of the returned value** correctly to the `get()` caller.
- **Join safely when dropped** (drop = join, per §3).
- **Propagate task failure at `get()`** (see *Errors*).
- **Reject unsafe captures** — independent-task args must be `own`/`copy`, never
  `view`/`mod` (grounded in §5's shallow-move finding).

**Explicitly out of scope for the first slice:** implicit auto-join coercion
(rejected outright), `taskScope`, `parallelLoop`, `Channel`, the C backend, and
any VM scheduler. Keeping the first slice this small makes it understandable and
testable on its own.

---

## 8. Errors & cancellation (to be specified)

- **Failure propagation:** a task that fails (panic / runtime error) stores
  `failed` status; the failure surfaces at `task.get()`. Exact mechanism depends
  on Rae's error model, which is an open question — pin it down before the C
  backend lands.
- **Cancellation:** `taskScope` exit cancels-or-joins remaining children. The
  cancel semantics (cooperative cancel points vs join-only) need definition; the
  first milestone is join-only (drop-joins), cancellation comes with `taskScope`.

---

## 9. Risks & open questions

- **Soundness of the spawn-boundary capture rules is make-or-break.** If the
  rules ever admit one shared mutable alias across an independent-task boundary,
  the program races. With no sequential reference engine any more there is
  no equivalence suite to fall back on — only sound static rules
  (`c_spawn_threadable` plus the sequential fallback for every unproven
  shape) prevent the race. ASan/UBSan runs of the spawn fixtures are the
  backstop.
- **Global runtime state** was audited and made thread-safe (§5), and the
  TSan gate keeps it so; a new mutable static in the runtime needs a lock, an
  atomic, or `__thread`.
- **`Task(T)` drop/lifetime** must integrate with cascade-drop / scope-exit
  dealloc (`docs/scope-exit-dealloc.md`): a dropped task joins, then its result
  slot is dropped.
- **Hot-reload with live tasks** — patching a chunk a task is mid-execution in.
  Likely: quiesce/await tasks across a reload, or forbid reload while tasks run.
- **`parallelLoop` disjointness proof** — the compiler must actually verify that
  per-iteration `mod` access is non-overlapping, or conservatively reject. The
  proof obligation is the hard part of the data-parallel path.
- **Per-spawn `vm_init`** (fresh sub-VM per task) is heavy; pool/reuse later for
  perf — not a v1 concern.
- **Final `parallelLoop` / range syntax** is unresolved and must be designed with
  Rae's eventual collection/range-loop syntax, not invented as a separate
  mini-language.

---

## 10. Style conformance

- Types are PascalCase: `Task`, `Channel`, `TaskGroup`, `Atomic`.
- Functions/methods are camelCase: `get`, `send`, `recv`, `tryRecv`, `joinAll`.
- `spawn`, `detach`, `taskScope`, `parallelLoop` are keywords / language
  constructs and read as such.
- Synchronization is always written out (`task.get()`); nothing blocks invisibly.
