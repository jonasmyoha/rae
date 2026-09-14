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
joins exactly once and yields the result; a `Task` local that goes out of
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
(`TYPE_TASK`) with one method, `get(): T`. `taskScope { … }` parses to a
run-once `if` block flagged `is_task_scope` (`parser.c`); `parallelLoop`
parses to a `loop` flagged `is_parallel`. Both flags are carried, formatted
(`rae format`, fixture 813) and otherwise ignored by codegen — `taskScope`
gets its semantics from join-on-drop, `parallelLoop` runs as a plain loop.

**Runtime (`compiler/runtime/runtime_threads.c`, `rae_runtime.h`).**
`Task(T)` lowers to `RaeTask*`: `{ pthread_t thread; void* result; int done;
int joined; }`, one pthread per spawn. `rae_task_await` joins once (guarded)
and hands back the result buffer, which the `get()` site casts to `T`;
`rae_task_drop` joins if needed and frees. There is **no** running/failed
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
worker takes a stop channel and is joined at teardown), a truly parallel
`parallelLoop` with disjointness checking, atomics, task failure status /
propagation, `taskScope` cancel-on-error / non-escape enforcement, and
threading the `mod`/`view`-aggregate shapes (they are correctly sequential).
The `g_mem_*` accounting counters are still plain globals (a benign,
stats-only race under threads). The staged plan for the rest is
`docs/parallelism-first-plan.md`.

---

## 6. Execution engine

There is one engine: the Compiled (C) backend. The Live/bytecode VM that the
first revisions of this section described ran `spawn` synchronously and was
removed in #957 — with it went the "Live runs sequentially first" staging and
the Live↔Compiled observable-equivalence suite. What the design asked of the
compiled engine, and where it stands:

- Real OS threads over pthreads — **done** for the threadable shapes (§5);
  there is no thread pool, one thread per task.
- `Task(T)` = heap struct with typed result slot — **done**; status
  (running/completed/failed) and a condition variable — **not yet** (join is
  `pthread_join`).
- `Channel(T)` = mutex-guarded queue — **done** as MPSC over any value type
  (#969); the MPMC + condvar (blocking receive) form is **not yet**.
- Atomics via C11 `<stdatomic.h>` — **not yet**.
- `parallelLoop` = genuine parallel execution over disjoint shards — **not
  yet** (sequential).

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
   `Task` runtime, channels, atomics, real `parallelLoop`; audit/fix the global
   runtime state (`g_mem_*`) first.
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
- **Global runtime state** (`g_mem_*`, interned pools, registry) must be
  audited and made thread-safe before real `parallelLoop`; the String temp
  pool is already `__thread`, the `g_mem_*` counters are still a stats-only
  race.
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
