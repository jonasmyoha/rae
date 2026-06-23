# Rae Concurrency & Threading — design

Status: **partially implemented** (VM / Live target). This document is the
authoritative model; implement against it in the staged order under *Roadmap*.

Implemented so far (Live / bytecode VM only):
- `spawn f(args)` returns a joinable `Task(T)` (replaces the old detached,
  result-discarding, handle-leaking prototype). `OP_SPAWN` allocates a
  ref-counted `TaskObj`, runs a non-detached thread, and the spawned
  function's return value is captured into the task's result slot.
- `task.get()` (`OP_TASK_GET`): joins once, yields the result. Verified with
  Int and String results (heap result moves correctly across threads) and
  two concurrent tasks.
- Dropping the last reference to a running task **joins it** (join-on-drop).
- Type system: builtin `Task(T)` (`TYPE_TASK`); `spawn`→`Task(T)` typing;
  `Task(T)` annotations resolve; `Task(T).get(): T`.
- **Capture safety:** a spawned function's `view`/`mod` params are rejected
  unless the borrowed type is a value type with no heap — scalars
  (Int/Float/Bool/Char) and enums. Borrowed String/List/struct/Buffer/etc. is
  a compile error (would alias the parent heap across threads; pass own/copy).
- **Failure propagation:** a task that fails (worker runtime error) is recorded
  as `status=failed`; `task.get()` raises instead of returning a bogus result.

Still TODO for the first milestone: make a bare `spawn f()` statement
join-on-drop instead of leaking the discarded `TaskObj` (the VM's `OP_POP`
doesn't free temporaries). Compiled (C) backend `spawn`, `Channel`,
`taskScope`, and `parallelLoop` are unstarted.

The design came out of a roundtable (Chattie / Clo / Gem) plus a final
maintainer pass. It deliberately diverges from the roundtable on two points:
**no implicit task→value coercion**, and **no early green-fiber VM scheduler**.

The design came out of a roundtable (Chattie / Clo / Gem) plus a final
maintainer pass. It deliberately diverges from the roundtable on two points:
**no implicit task→value coercion**, and **no early green-fiber VM scheduler**.

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

Two execution backends, one language semantics:

- **Compiled (C):** genuine parallelism (thread pool, real OS threads).
- **Live (bytecode VM):** same ownership checks and same observable results,
  but initially sequential / cooperative — Live makes **no promise** about
  timing or real parallel performance.

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

## 5. Current state (what exists today)

`spawn` is already wired through the front end and the VM, but it is a
**detached prototype**, not a task model:

- Parser: `AST_UNARY_SPAWN`; sema; VM codegen via `is_spawn` in
  `compiler/src/vm_compiler.c`.
- VM runtime: `OP_SPAWN` in `compiler/src/vm.c` spawns a **fresh sub-VM on a
  detached OS thread** (`spawn_thread_wrapper` + `SpawnData`), via the
  cross-platform `sys_thread` abstraction.
- **It never joins** — the `sys_thread_t` handle is discarded (a thread leak),
  there is **no result path**, and there is **no C-backend codegen** for spawn
  at all (VM-only).
- `docs/multiplayer-highscore-plan.md` already commits the project to the
  "threads-over-async, `spawn`" direction.

**First soundness question — answered.** `OP_SPAWN` transfers arguments with
`data->args[i] = vm_pop(vm)`: a **shallow, by-value move of the `Value` struct**
off the parent stack. Heap payloads (String / List buffers) are **not**
deep-copied — only the pointer-bearing `Value` is moved, and the parent
relinquishes its stack slot. Consequences that the model must enforce:

- It is safe **only** when the argument was genuinely `own` (the parent no
  longer references that heap). This is exactly why §2a restricts independent-
  task captures to `own`/`copy`.
- A `view`/`mod` capture, or an `own` value the caller still aliases elsewhere,
  would share the heap across threads → data race and/or double-free at drop.
  The compiler must reject these at the `spawn` boundary.

This is therefore an **isolate-per-task** prototype to *evolve* (make joinable,
result-returning, ownership-checked), not to replace.

A second runtime hazard to fix before enabling real parallelism: the runtime's
global memory-accounting counters (`g_mem_*` in `compiler/runtime/rae_runtime.c`)
and any interned-constant/string pools are shared process-globals and would race
under multiple OS threads — make them atomic or per-thread.

---

## 6. Live vs Compiled execution

Same language semantics and same ownership checks on both; different engines.

**Compiled (C):**
- Real thread pool over `sys_thread` / `sys_mutex`.
- `Task(T)` = heap struct: typed result slot + status (running/completed/failed)
  + condition variable + owned thread handle.
- `Channel(T)` = mutex + condvar MPMC queue.
- Atomics via C11 `<stdatomic.h>`.
- `parallelLoop` = genuine parallel execution over disjoint shards.

**Live (bytecode VM):**
- Initially **sequential or cooperatively scheduled** tasks.
- `parallelLoop` may execute **sequentially** — identical observable results,
  no parallelism.
- Same ownership checks; **no promise** of reproduced timing or real parallel
  performance.
- **Do not build a sophisticated green-fiber VM scheduler up front.** That is a
  large project and is not needed to establish the language model. Reconsider a
  real VM task scheduler only when Live genuinely needs responsive interleaving
  (step 5 below).

---

## 7. Roadmap (staged, in order)

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
  Compiled can race even where Live (sequential) looks fine. The Live↔Compiled
  equivalence suite is the backstop, but only sound static rules prevent the
  race in the first place.
- **Global VM/runtime state** (`g_mem_*`, interned pools, registry) must be
  audited and made thread-safe before step 4.
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
