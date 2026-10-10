# Lightweight `spawn`: design

**Status:** S1 (measured, §10), S2 (the may-wait report, §11) and S3 (the
scheduler, S3a, §12; the resumable codegen, S3b, §13) are done. Of S3c (§14),
the waiting thread now computes too; lending a `view` to a task is a
proposal waiting for the maintainer's decision. S4 and S5 are next. The tasks take §9's recommendations as working decisions unless the
maintainer changes a fork here.

- S1 and S2 (measure; the may-wait report) sit right after the server phase 1
  task.
- S3–S5 (codegen and scheduler; socket waits and the browser; blocking externs
  and fairness) sit after the server phase 3 task.

**In one paragraph.** Rae keeps its "reversed await" syntax: concurrency is
marked at the call site with `spawn`, functions are never coloured, and
waiting is an explicit `task.get()`. What changes is the machinery
underneath. Today every `spawn` is an OS thread and every wait blocks a
thread. This design makes a `spawn` a lightweight task: a compiler-generated,
resumable state machine that the worker pool runs M:N across the cores, and
that *suspends* instead of blocking when it waits. That is Go's model
(`go f()`, no `async`, cheap goroutines), implemented the portable way Rust
and C# compile `async` (as stackless state machines), and inferred by the
compiler instead of declared, which is possible because Rae compiles the whole
program and has no function pointers.

## 1. Today

From `docs/concurrency-model.md` and `compiler/runtime/runtime_threads.c`:

- `spawn f(args)` → `Task(T)`, **one pthread per spawn**. Arguments are deep
  copied (or moved) into the thread. A call whose arguments cannot be captured
  (a `mod` / `view` of a non-scalar) runs synchronously into an
  already-completed task instead.
- `task.get()` joins once and **blocks** the calling thread; a `Task` dropped
  without `get()` is joined on drop; `taskScope { }` joins its tasks at exit.
- `Channel(T)` is MPSC and non-blocking (`pending`, `receive`).
- `parallelLoop` runs on the **worker pool** (performance cores minus one, at
  the launcher's QoS); `spawn` does not use the pool.
- Things that wait today: `task.get()`, `sleep(ms:)`, `socketWaitReadable`,
  `tcpConnect`'s bounded wait, blocking file I/O, and the poller's wait once
  G2 lands.

What that costs: a thread per spawn takes tens of microseconds to start and a
stack each. Ten thousand concurrent waits means ten thousand threads, and every
wait parks a whole thread. The syntax is already right; the machinery is not
cheap enough to use freely.

## 2. How other languages do it

| language | marked where | colouring | a task is | executor | wait blocks the thread? |
|---|---|---|---|---|---|
| **Go** | call site, `go f()` | none | stackful goroutine (~2 KB, growable stack) | M:N over all cores, work-stealing, network poller, preemption | no |
| **Java 21** | `Thread.ofVirtual().start(...)` | none | stackful virtual thread | M:N on a ForkJoinPool | no (most calls) |
| **Zig 0.6–0.10** | call site, `async f()` | none: inferred by whole-program analysis | stackless frame | library event loop | no; removed in 0.11 pending a redesign |
| **Rust** | declaration, `async fn`, plus `.await` | yes | stackless future, lazy (runs only when polled) | a library (tokio: multi-threaded, work-stealing) | no |
| **C#** | declaration, `async`, plus `await` | yes | stackless state machine | thread pool | no |
| **Kotlin** | declaration, `suspend fun` | yes | stackless continuation | dispatchers (Default is multi-threaded) | no |
| **Dart** | declaration, `async`, plus `await` | yes | eager future | ONE event loop per isolate; parallelism = isolates sharing no memory | no |
| **JavaScript** | declaration, `async`, plus `await` | yes | eager promise | ONE event loop; workers are separate | no |
| **Rae today** | call site, `spawn` | none | an OS thread | none (a pthread each) | **yes** |

Two lessons:

- **Go** shows that call-site marking with no colouring works at the largest
  scale.
- **Zig** shows that the compiler can infer which functions must be resumable,
  so nobody writes `async`. It also shows where that gets hard: function
  pointers and virtual calls hide the call graph. Rae has neither.

## 3. The proposal

**Syntax: unchanged.** `spawn f(args)`, `task.get()`, `taskScope { }`,
`Task(T)`, `Channel(T)`. No new keyword for the common case (fork F4).

**Semantics:**

- `spawn` creates a lightweight task and queues it on the scheduler; it does
  not create a thread.
- Inside a task, a wait **suspends the task**: `get()` on another task,
  `sleep`, a socket wait, a channel wait if one is added. The worker thread
  then runs another ready task, and the suspended one resumes when its
  condition is met.
- Outside any task (`main`, or a `mainLoop` frame), a wait blocks the thread
  exactly as today. `main` is a thread, not a task (fork F5).
- Parallelism is kept: the scheduler runs tasks on every worker, so
  CPU-heavy spawned work still uses all cores.
- Observable results are the same as today. The argument-capture rules
  (deep copy, move, the synchronous fallback for non-capturable arguments)
  are unchanged.

**What a program gains:** spawning ten thousand waiting tasks is cheap, and a
straight-line function that waits (`let a: Int = fetchA(); let b: Int =
fetchB(); ret a + b`, called through `spawn`) suspends at each wait instead of
parking a thread. For a server, that is how "call service A, then B, then
reply" stays straight-line code instead of a hand-written state machine in
components.

### 3.1 Fork-join recursion: the lesson from Bend

**Bend** (HigherOrderCO, Victor Taelin) is a language built around massively
parallel divide-and-conquer.

- **Bend 1** (2024, on the HVM2 runtime) parallelised everything implicitly.
  It evaluated programs as interaction nets over immutable data, with no
  annotations at all. Divide-and-conquer code then spread over thousands of
  CPU or GPU threads. Its cost was single-thread speed: by its own README and
  independent benchmarks, 10–50x slower than C, with linked-list arrays
  (O(i) indexing) and high memory use.
- **Bend 2** ("a new language; Bend 1 programs and HVM do not carry over")
  reverses most of that:
  - "everything is annotated and nothing is inferred";
  - parallel and GPU execution are marked at the call (`pow2!(20n)`);
  - values are **affine** (closures and arrays cannot be shared);
  - it has dependent types and machine-checked proofs (`LAWS.bend`);
  - it compiles to C, Metal, CUDA and JavaScript;
  - its README *claims* hand-written-C speed on one core (no published
    numbers there yet).

Its model of parallelism is the same idea as Bend 1's, now explicit: "split
the work in two, and Bend spreads the calls over every core it can find, then
joins them back."

**What Rae takes from it.** Bend 2 has converged on what Rae already has: an
explicit marker at the call site, plus values with one owner. Rae's version
is `spawn` with `own`/`copy` arguments and frozen `let`s. What Bend does that
Rae cannot do cheaply today is **fork-join recursion**, that is splitting in
two, solving both halves concurrently and combining:

```rae
func parallelSum(numbers: view List(Int), start: copy Int, count: copy Int) ret Int {
  if count < 4096 {
    ret sequentialSum(numbers: numbers, start: start, count: count)
  }
  let half: Int = count / 2
  let left: Task(Int) = spawn parallelSum(numbers: numbers, start: start, count: half)
  let right: Int = parallelSum(numbers: numbers, start: start + half, count: count - half)
  ret left.get() + right
}
```

(A sketch that shows the real open point. Today a `spawn` whose argument is
a `view` of a non-scalar runs synchronously instead (docs/concurrency-model.md),
so this exact code would not run in parallel. The halves must either get
their own copy, or the language must let tasks inside a `taskScope` share
read-only data that outlives them. Bend 2 sidesteps the question because its
values are affine. S3 has to decide it, and measure the cost of copying
against the cost of sharing.)

With an OS thread per `spawn`, a recursion like this costs a thread per split,
and `get()` parks a whole thread, so it only pays off for very coarse
splits. With lightweight tasks on a work-stealing pool (§4.3) it becomes the
cheap, natural way to use every core. The pattern goes back to Cilk; it is
how Go, Rayon (Rust) and Bend all do it.

So fork-join recursion is a **first-class target** of this design:

- S1 benchmarks it with today's threads;
- S3 must make it scale: near-linear speedup on 8 cores for a recursion down
  to small leaves, with no hand-tuned cut-off beyond a leaf size.

**What Rae does not take:**

- implicit parallelism (Bend 1's, which Bend 2 itself dropped);
- interaction-net runtimes and pointer-heavy data (Rae is data-oriented:
  dense lists and ECS tables);
- the proof system.

A possible later construct, "run these independent calls in parallel" with
independence proven from parameter modes (no shared `mod` argument), is fork
F9: new syntax, only if `taskScope` + `spawn` reads too heavily once spawn is
cheap.

## 4. Implementation: stackless, inferred

### 4.1 Which functions become resumable (the inference)

1. **Wait primitives** are a closed, compiler-known list: `Task.get`, `sleep`,
   the socket/poller waits, a blocking channel receive (if added).
2. **May-wait** propagates up the call graph: a function may wait if it calls
   a wait primitive or a may-wait function. Rae compiles the whole program and
   has no function pointers, so the call graph is complete and the analysis is
   one fixed-point pass.
3. Only may-wait functions reachable from a `spawn` get a **resumable twin**.
   Everything else is compiled exactly as today, and the normal version of a
   may-wait function is still emitted for non-task callers (where a wait
   blocks).

Most code is never transformed: a parser, a solver or a renderer does not
wait. Generic functions are specialised first, then analysed per
specialisation, the same order the backend already uses.

### 4.2 What a resumable function compiles to

The C backend emits, for each resumable function, a **frame struct** and a
**step function**:

- the frame holds the parameters, every local that lives across a wait, the
  child frame of the call in progress, and a state number;
- the step function is a `switch (frame->state)` that jumps to the code after
  the last wait, runs until the next wait, stores the state and returns
  "suspended" (or "done" with the result).

This is how Rust and C# compile `async`, and it is plain C: no assembly, no
stack switching, and it runs unchanged in wasm. The C compiler optimises the
straight-line parts as usual.

Frames are **heap-allocated once per call and never move**. A borrow (`view`
/ `mod`) held across a wait therefore stays valid: a child frame may point
into its parent's frame, because the parent is suspended waiting for that
child and its storage stays where it is. Rust needs `Pin` because its futures
can move; Rae's frames cannot. Two things stay ruled out, both of which
already are today:

- borrows crossing a TASK boundary (`spawn` copies or moves its arguments);
- a `view`/`mod` stored into user data.

Frame allocation is per call of a resumable function, not per task. It can be
pooled per worker (fork F6), so a steady-state server allocates nothing.
Recursion through a resumable function allocates a frame per level: correct,
just not free.

### 4.3 The scheduler

- **Workers:** the existing pool (performance cores minus one, at the
  launcher's QoS), each with a run queue of ready tasks; idle workers steal
  from busy ones. A task is a pointer to its root frame plus a state.
- **Waking:**
  - a completed task wakes the task waiting in its `get()`;
  - `sleep` puts the task on a timer heap;
  - a socket wait registers interest with the poller (G2), and a
    readiness thread (or the worker that finds the run queue empty) polls
    kqueue/epoll and makes the waiting tasks ready.
- **Runtime C:** only what the platform forces (the rule in AGENTS.md): the
  threads, an atomic run-queue operation, the poller shim. The scheduler's
  policy (queues, stealing, timers) is Rae or generated code where possible
  (fork F7).
- **The browser:** one worker. Tasks interleave on the main thread between
  frames (`mainLoop`'s frame callback runs ready tasks), which works because
  the tasks are stackless. Wasm cannot switch stacks.

### 4.4 Calls that block in C

An `extern` C call may block for a long time (a `read` on a file, a library
that does network I/O itself). A task stuck in one would hold its worker. The
options (fork F2):

- (a) Mark such externs, with a bare keyword as for every Rae modifier (no
  `@`). A call to one inside a task runs on a dedicated blocking thread while
  the task suspends.
- (b) Go's approach: the scheduler notices a worker stuck in a call and hands
  its queue to a fresh thread. That needs no marking, but it is more runtime
  machinery.
- (c) Nothing: document that blocking externs inside tasks stall a worker.

**Decided (maintainer, 2026-10-08): (a), with the keyword `blocking`.** It is
written where Rae puts declaration modifiers, before `unsafe extern`, and is
valid ONLY on `extern` declarations:

```rae
# may wait on the disk for a long time: inside a task it runs on a blocking-call thread
func nativeReadFile(path: String) blocking unsafe extern("rae_ext_Files_read") ret String

# never waits (sockets are non-blocking; the poller does the waiting): no mark
func nativeRead(handle: Int, buffer: Buffer(UInt8), offset: Int, maxBytes: Int) unsafe extern("rae_ext_Net_read") ret Int
```

- **Why this word:** "blocking call" is the standard term (Rust's tokio has
  `spawn_blocking` for exactly this). It is a plain keyword, never an `@`
  attribute.
- **Who uses it:** with the Rae-first rule almost every extern lives in
  `lib/`, so the stdlib authors mark the few C calls that can wait (file I/O,
  a library doing its own network calls). The generated WebGPU and Box3D
  bindings do not block and stay unmarked.
- **Meaning:** inside a lightweight task, a call to a `blocking` extern runs on
  a separate blocking-call thread while the task suspends. Outside a task (in
  `main`, a `mainLoop` frame, or with today's thread-per-spawn) it changes
  nothing, and the call just runs.
- **Rules:** sema rejects `blocking` on a non-extern function; `rae format`
  prints it; it is part of the extern's declaration, not of its type.

### 4.5 Fairness

A task that computes for a long time without waiting keeps its worker. Go
preempts goroutines; a stackless design can only yield at points the compiler
inserts. The options (fork F3):

- (a) Cooperative only: a CPU-heavy task holds its worker, and the other
  workers continue. This is simple, and matches today's threads on a busy
  machine.
- (b) The compiler inserts a cheap "yield if your slice is used up" check at
  loop back-edges in resumable functions.

## 5. What it does not change

- **The server design** (docs/server-benchmarks-design.md) does not depend on
  this. I/O goes through the poller; slow work is spawned and picked up with
  `tryGet` (T1). Lightweight spawn would let a later version write per-request
  flows as straight-line spawned functions instead of `AwaitingResult` state.
  The phase 1–3 benchmarks are the evidence for whether that is worth it.
- **`parallelLoop`**, `taskScope`, `Channel`: same syntax and semantics;
  `taskScope` becomes cheap.
- **Determinism and TSan:** tasks still share nothing (arguments are copied
  or moved), so the race-freedom argument of docs/concurrency-model.md is
  unchanged. TSan runs the scheduler like any threaded code.

## 6. Costs and risks

- **Compiler work:** the may-wait analysis is small; the frame and step code
  generation is the real cost (live-variable analysis across waits, drops of
  a suspended frame's locals, `ret` inside loops, `defer` across waits).
- **Code size:** resumable twins of may-wait functions only; usually a small
  share of a program.
- **Debugging:** a suspended task has no native stack. A debug build can keep
  a frame-chain walker ("task stack") for logs and crash reports.
- **Dropping a suspended task:** a `Task` dropped while its frames are
  suspended must drop those frames' live locals (the frames are known
  structs, so this is generated code, like the cascade drops Rae already
  emits).
- **Two behaviours of one wait** (suspend inside a task, block outside) are
  what Go, Java and Zig do as well, but they must be documented where waits
  are documented.

## 7. Alternatives considered

- **Stackful tasks (Go, Java 21).** No compiler transform; any function can
  wait. But it needs per-architecture stack-switching assembly in the
  runtime, growable or guarded stacks, and it cannot run in wasm without
  Asyncify/JSPI. Rae compiles to C and targets the browser, so the stackless
  route fits better.
- **`async`/`await` keywords.** Rejected by docs/concurrency-model.md:
  colouring spreads through the code and adds a second vocabulary for calls.
- **Keep OS threads.** That is the status quo. It is fine while spawns are
  few (the server design uses the poller for I/O). It stays the baseline the
  benchmark compares against.

## 8. Phasing (if approved)

1. Measure first: the server phases 1–3 with thread-per-`spawn`, plus
   microbenchmarks (spawn and join 100 000 tasks; 10 000 concurrent sleeps;
   fork-join recursion, §3.1, against its sequential version).
2. The may-wait analysis, with a report only (`rae build --report-waits`):
   which functions would become resumable, and how many. **Done (S2, 0.1.224): see §11.**
3. Resumable codegen for `Task.get` and `sleep` only, on the existing worker
   pool, with fixtures (a deep chain of waits, a wait inside a loop, a borrow
   across a wait, dropping a suspended task, leak-checked; TSan). **Split in
   two. S3a, the scheduler, is done (0.1.251, §12):** spawns that wait only on
   other tasks run M:N on the pool, with the policy in Rae. **S3b**, the frame
   and step codegen, makes `get()` and `sleep` suspend: done (0.1.252, §13).
4. Socket and poller waits (after G2) and the browser path. **Socket waits:
   done (0.1.256, §15).**
5. The extern-blocking and fairness choices (F2, F3).

## 9. Decisions for the maintainer

1. **F1 Implementation:** stackless inferred state machines (recommended:
   portable C, works in wasm, fits the whole-program compiler) or stackful
   tasks.
2. **F2 Blocking externs: DECIDED** (maintainer, 2026-10-08). The `blocking`
   keyword on extern declarations (§4.4).
3. **F3 Fairness:** cooperative only first (recommended), with loop
   back-edge yield checks as a later option if measurements show starvation.
4. **F4 One keyword:** `spawn` becomes lightweight and stays the only spelling
   (recommended, as Go has only `go`), or keep a separate OS-thread spawn for
   special cases.
5. **F5 `main`:** stays a thread where waits block (recommended), or runs as
   the first task.
6. **F6 Frame pooling:** per-worker frame pools from the start, or plain
   allocation first and pooling when measured (recommended: measure first).
7. **F7 Scheduler code:** as much of the scheduler as possible in Rae over
   minimal C (threads, atomics, poller), per the runtime rule (recommended),
   versus a C scheduler.
8. **F8 When:** after the server phase 1 numbers exist (recommended), or now. Measured in §10: the numbers justify continuing with S2.
9. **F9 A parallel-calls construct:** later, and only if `taskScope` +
   `spawn` reads too heavily for fork-join once spawn is cheap: a construct
   that runs independent calls in parallel, with independence proven from
   parameter modes (§3.1). It is new syntax, so it needs approval. Not now
   (recommended).

## 10. Measured (S1, 2026-10-08)

`benchmarks/spawn/` (`run.sh`, results in `benchmarks/spawn/results/`)
measures today's thread-per-`spawn` next to tokio tasks. Go is not installed
on the machine, so it is skipped.

- Machine: Apple M1 Max (8 performance cores), Rae release build.
- The machine was busy (desktop use, load average about 7 on 10 cores). The
  costs per spawn and the limits do not depend on that. The 8-way speedups
  are probably a little pessimistic for both Rae and tokio.

### Spawn, wait, latency

| | Rae today (a thread per spawn) | tokio (8 workers) |
|---|---|---|
| spawn + `get`, one at a time | 22.3 µs | 8.0 µs |
| spawn + `get`, batches of 256 | 14.7 µs per task | 0.23 µs per task (63x less) |
| spawn → task running, p50 / p99 | 9.3 / 33.9 µs | 2.1 / 12.2 µs |
| spawn → `get` returned, p50 / p99 | 19.7 / 59.6 µs | 4.8 / 23.5 µs |
| 10 000 concurrent 100 ms sleeps | works: 196 ms wall, **1.26 GB** RSS | 110 ms, 4.8 MB |
| tasks alive at once, at most | **16 383** (macOS `kern.num_taskthreads` 16 384, minus the main thread), about 126 KB RSS each: 2.0 GB at 16 000 | 1 000 000 sleeps: 766 ms, 255 MB |

- **Past the limit, `spawn` fails silently.** The generated C ignores
  `pthread_create`'s result, so a task that never started still returns
  from `get()`, with a result that was never computed. With 17 000, 20 000 or
  30 000 tasks alive at once, exactly 16 383 returned their value and the
  rest returned 0 without an error. That was a correctness bug independent
  of this design. **Fixed (0.1.233):**
  - the spawn site calls `rae_task_start`, which runs the task on the caller
    when no thread can be had, as an uncapturable spawn already does, and
    says so once on stderr;
  - 17 000 and 20 000 `held` tasks now all finish;
  - fixture 1061 hits a test-only cap (`RAE_SPAWN_THREAD_CAP=4` in its
    `config.env`) and is checked under TSan.
- **Spawning slows down while threads are exiting.** For 10 000 sleeps,
  spawning took 92 ms. For 15 000 it took 502 ms, and for 50 000 it took
  9.3 s. Those runs never had all their tasks alive at once, because the
  first ones had finished before the last were spawned. With every thread
  alive (`held`), 15 000 spawns take 143 ms.

### Fork-join recursion (§3.1)

**Merge sort** of 2 000 000 Ints. The halves are copied, in Rae and tokio
alike. Sequential: Rae 180 ms, Rust 108 ms.

| leaves (Rae: threads) | 1 | 2 | 4 | 8 | 64 | 1 024 | 4 096 |
|---|---|---|---|---|---|---|---|
| Rae speedup | 1.00 | 1.75 | 3.15 | 4.56 | 4.37 | 2.82 | **0.47** |
| Rae peak RSS | 128 MB | 210 MB | 290 MB | 360 MB | 538 MB | 840 MB | 1.0 GB |

tokio, 1 024 leaves on 1/2/4/8 workers: speedup 1.00 / 1.68 / 2.98 / 4.64,
at 260–280 MB.

- With one thread per core (8 leaves), today's spawn scales as well as tokio
  (4.56x against 4.64x).
- Splitting finer costs Rae what it costs nothing in tokio. At 1 024 leaves
  Rae falls to 2.82x, and at 4 096 leaves it is **slower than sequential**,
  with 1 GB of thread stacks. That is §3.1's "only pays off for very coarse
  splits", now measured. The leaf size has to be hand-tuned to the core
  count, which is exactly what S3 is meant to remove.

**Sum** of 16 000 000 Ints. Sequential: Rae 6.8 ms, Rust 2.4 ms.

| | speedup at 1 / 2 / 4 / 8 |
|---|---|
| Rae fork-join, leaves = 1 / 2 / 4 / 8 (each half **copied**) | 0.57 / 0.31 / 0.25 / 0.23 |
| Rae fork-join, 64 / 1 024 / 4 096 leaves | 0.20 / 0.14 / 0.02 |
| Rae `parallelLoop` (shared `view`), `RAE_WORKERS` 1 / 2 / 4 / 8 | 1.12 / 2.11 / 4.00 / 5.01 |
| tokio fork-join (shared `Arc`), 1 024 leaves, workers 1 / 2 / 4 / 8 | 0.71 / 1.02 / 1.51 / 1.87 |

- One copy of the input alone takes 11.3 ms, more than the whole sequential
  sum. A fork-join that must copy its halves can never win on a cheap leaf:
  even with 2 threads it is 3x slower than sequential.
- The same work over shared data (`parallelLoop`) is 5x faster than
  sequential. So for fork-join over data, **the copy is the bigger cost,
  not the thread**, and a lightweight spawn alone would not fix it.
- This is the open point of §3.1 in numbers. S3 must let tasks inside a
  `taskScope` read data that outlives them (a `view` into the scope's
  owner); copying is not a viable default.
- Rae's sequential loops are 1.7–2.8x slower than Rust's here (the
  per-element `copyAtFallback`). That is the list-access question in
  `benchmarks/list_access`, separate from spawn.

### The servers (phase 1, `benchmarks/servers/results/`)

- **Thread-per-spawn costs phase 1 nothing.** Both Rae HTTP servers run one
  thread per worker and spawn nothing per request.
- With 1 worker, the event-loop and ECS servers do 134k / 138k
  plaintext requests/s, 128k / 133k json, and 1.23M / 1.24M pipelined. That
  is 8–16% above hyper on tokio.
- The cost would appear only for a request that `spawn`s slow work (the ECS
  server's `AwaitingResult`, §7 of the server design). At 22 µs and 126 KB
  per spawn, that works for a few slow requests and fails at a few thousand
  waiting at once.
- Phases 2–3 (thousands of WebSocket clients) do not spawn per client in
  either style either: they are poll-driven.

### Does this justify continuing (F8)?

**Yes, but not because the servers need it.** The measured case for S2/S3:

1. **Many concurrent waits do not fit.**
   - 10 000 sleeping tasks need 1.26 GB, where tokio needs 4.8 MB.
   - There is a hard limit at 16 383, and past it spawn fails silently.
   - "Ten thousand concurrent waits" (§1) is at the edge today, and a
     hundred thousand is impossible.
2. **Fork-join only works coarse.**
   - At one thread per core, the threads are fine.
   - At the leaf sizes a recursion naturally reaches, Rae becomes slower than
     sequential, while tokio keeps its speedup.
3. **Spawn itself costs 3–60x what a task costs.** That is 3x one at a time
   and 63x in a batch. It rules out spawning per request or per item.

And two findings that change the plan:

- **Sharing read-only data is as important as cheap tasks.** In the sum, the
  copy costs more than the work it splits, so S3's lightweight spawn must
  come with `view` access to data owned outside the `taskScope`. Otherwise
  fork-join over data stays slower than one core.
- **The silent `pthread_create` failure needs fixing now, whatever S2/S3
  decide.** A failed spawn should fail loudly (or run synchronously, as an
  uncapturable spawn already does), not return garbage.

Recommendation:

- go ahead with S2, the report-only may-wait analysis. It is cheap, and it
  sizes S3.
- give S3 the shared-`view` requirement above as a co-goal.
- the servers phase 2–3 work does not have to wait for any of it.

## 11. The may-wait report (S2, 2026-10-08)

`rae build --report-waits <Main.rae>` runs §4.1's analysis and prints which
functions would get a resumable twin. It emits nothing and changes no
codegen. `--report-waits-lib` adds lib/'s counts and lists lib/'s twins with
their lines. The code is `compiler/src/may_wait.c`; fixtures 1041 (a wait
three calls deep), 1042 (a generic that waits only for some T) and 1043
(self and mutual recursion).

**How it works:**

- **Wait primitives:** a closed list.
  - `task.get()`.
  - The externs that block: `sleep` / `Time.waitUntil` (`rae_ext_rae_sleep`,
    `rae_ext_Time_sleepNs`) and the socket and poller waits
    (`rae_ext_NetSys_pollOne`, `rae_ext_NetSys_pollerWait`).
  - The implicit joins: a `taskScope` end, and a Task local joined when it
    goes out of scope. These are waits too, and they are easy to miss: the
    recursive fork-join `parallelSum` waits through its `left` Task.
  - The `blocking` externs (F2) join the list when that keyword exists.
- **Generics after specialisation:** each instantiation the backend's
  discovery pass makes is its own node, walked with its type parameters
  bound. So `process(Slow)` gets a twin and `process(Fast)` does not
  (fixture 1042).
  - Inside a generic body, sema leaves a call unresolved when it depends on
    `T` (the app's `route(router:)` in `dispatchSystem(R)`). Such a call is
    resolved by name, by the concrete type of its first argument, and by
    inferring its type arguments from the arguments.
  - What is still undecided goes to every candidate, which
    over-approximates. The report counts those calls. Today they are all in
    container code (`drop`, `remove`, `swapRemove` of lists and tables),
    which never waits, so they cannot create a false twin; they only widen
    "reachable from a spawn".
- **The fixed point:** may-wait propagates to callers until nothing changes.
  Recursion and cycles need nothing special (fixture 1043).
- **Twins:** a twin is a function that may wait AND is reachable from a
  `spawn`. `main` stays a thread (F5), so waits that only `main` reaches
  (106's 36 may-wait program functions) are compiled as today.

**Counts** (`--report-waits-lib`, 2026-10-08):

| program | functions (program + lib/) | may wait | spawn sites | twins: program + lib/ | the twins |
|---|---|---|---|---|---|
| 106_mobile_ui | 472 + 4 929 | 36 + 53 | 3 | 1 + 2 | `spotifyPollWorker -> sleep` |
| server, event loop (`benchmarks/servers/rae/eventLoop/http`) | 10 + 446 | 2 + 10 | 1 | 1 + 2 | `serve -> pollWait -> systemPollerWait` |
| server, ECS (`benchmarks/servers/rae/ecs/http`) | 4 + 790 | 2 + 16 | 1 | 1 + 7 | `serve -> httpServerFrame(BenchRouter) -> pollSystem -> pollWait`; `closeSystem -> destroyConnection -> joinAwaiting` (the `AwaitingResult` join) |
| `benchmarks/spawn` | 26 + 333 | 12 + 5 | 7 | 4 + 4 | the sleepers, `waitUntil`, and the fork-join recursions (task joins) |
| 114_walker_character, 97_tetris3d, 124_vehicle_port | — | — | 0 | 0 | no spawns |

**What the numbers say for S3:**

- **The transform is small.** In every program measured, at most 8 functions
  of up to 5 400 need a twin. The rest (parsers, layout, the renderer,
  physics) never waits and is compiled exactly as today. That is §4.1's
  premise, now measured.
- **The servers' twins are the frame loop itself.** The ECS server's
  `httpServerFrame(BenchRouter)` and `pollSystem` become resumable because
  the worker loop waits in `pollWait`. That is how S3 removes a thread per
  worker: the poll wait becomes a task suspension.
- **The `AwaitingResult` join needs care.** It turns `closeSystem` and
  `destroyConnection` into resumable functions. A connection closed while
  its slow request is still running waits for the task inside a system. S3
  should keep that, or change `destroyConnection` to detach and drop the
  task instead of joining it.
- **106 needs only its poller worker transformed.** That is one function
  plus `sleep`.

## 12. The scheduler (S3a, 2026-10-09)

S3 was split into the scheduler (S3a, this section) and the resumable codegen
(S3b). S3a puts spawned tasks on the worker pool, M:N, for every spawn that
can run there without suspending.

**Which spawns run on the pool.** The C backend keeps §11's graph after
discovery and asks `may_wait_spawn_on_pool` per spawn. A spawn goes on the
pool when:

- it could run on a thread at all (`c_spawn_threadable`: its arguments are
  copied or moved);
- nothing it can reach waits except through task joins (`get`, a Task dropped
  at the end of its scope, a `taskScope`);
- every extern it can reach is the runtime's own kind (`lib/core/`, Math,
  Parallel, Time and Channel, minus the sleeps).

Every other spawn is still a thread, exactly as before:

- a task that sleeps waits for S3b, whose frames let it suspend;
- one that waits on a socket waits for S4;
- one that calls other C (files, a library doing network calls) waits for
  the `blocking` keyword (S5).

`RAE_SPAWN_THREADS=1` at build time keeps every spawn a thread, for
comparisons.

**How a task waits without suspending.** A worker that waits on a task runs
other tasks meanwhile (help-first, as Cilk's and Rayon's `join` do): its own
newest, then the injection queue, then a stolen one. So a fork-join recursion
keeps every worker busy, and a waiting worker never holds up the task it waits
for. Helping nests on the worker's stack, so pool threads get a 64 MB stack
(address space; untouched pages cost nothing). Fixture 1073 runs a chain of
10 000 nested waits. A thread off the pool (`main`) that waits on a task
nobody has started takes it back out of the injection queue and runs it
itself, so spawn-then-get costs no wake-up. Otherwise it blocks until the
task is done. That is the help-first fallback S3b keeps for code that is not
transformed; S3b adds real suspension for `sleep` (and S4 for sockets).

**Rae policy, C kernel (F7, docs/runtime-c-audit.md row 13).**

- `lib/core/Scheduler.rae` holds the policy:
  - the worker loop: parallelLoop chunks first, then its own newest task,
    then the injection queue, then stealing the oldest task of the next
    worker on; then 250 µs of spinning, then park;
  - where a new task goes (the spawning worker's own queue, or the injection
    queue from outside the pool; with no pool threads it runs on the spot);
  - how a task is waited for;
  - the default worker count (`RAE_WORKERS`, else performance cores minus
    one);
  - a parallelLoop's chunk size (four chunks per worker).
- The generated `main` installs these as the runtime's hooks
  (`rae_sched_install`).
- `runtime_threads.c` keeps only what C must do, one call per operation:
  - a locked task queue per worker plus the injection queue (push, pop
    newest, pop oldest, take back, an atomic count to look without locking);
  - parking a worker on an epoch, and waking one or all;
  - running a task's thunk, and blocking a thread until a task is done;
  - starting the pool's threads at the launcher's QoS;
  - the parallelLoop claim words.
- Its C copies of the worker-count and chunk rules and its old C worker loop
  remain only for a program built without the prelude.
- The parallelLoop pool and the task pool are one set of threads.
  `benchmarks/parallel_stages` (220 parallelLoops per step) runs the same
  with the Rae worker loop as with the old C loop: 0.96–1.28 ms against
  1.15–1.24 ms per staged step at 8 workers, on a machine at load 5.

**Measured (`benchmarks/spawn`, M1 Max, 8 workers).** The before is this
build with `RAE_SPAWN_THREADS=1`, kept in `results/threadPerSpawn/` (load
4.65). The after is `results/` (load 11.9: the machine was busy, so the
after is if anything pessimistic).

| | thread per spawn | scheduler (S3a) | tokio |
|---|---|---|---|
| spawn + `get`, one at a time | 22.2 µs | 3.2 µs | 8.6 µs |
| spawn + `get`, batches of 256 | 13.5 µs per task | 3.3 µs per task | 0.22 µs |
| spawn → `get` returned, p50 / p99 | 21 / 63 µs | 0.17 / 45 µs | 8.0 / 34 µs |
| merge sort 2 M, 1 024 leaves: speedup | 2.82x | 4.37x | 4.52x |
| merge sort 2 M, 4 096 leaves: speedup | **0.56x** | **4.14x** | — |
| sum 16 M, 4 096 leaves | 331 ms | 45 ms | — |

- **Fork-join no longer has to be tuned to the core count.** At 4 096 leaves
  the thread-per-spawn sort was slower than sequential, with 1 GB of thread
  stacks. On the pool it keeps its speedup at any leaf size, like tokio's.
  The leaf size is the only cut-off.
- **The speedup is not yet near-linear: 4.4x on 8 cores, as tokio's.** Two
  things hold it there:
  - the sort copies its halves at every level, which is memory-bound;
  - the waiting `main` is not a worker, so 7 threads compute.
- **The sum is still bound by copying** (§10): one copy of the input costs
  more than the whole sequential sum. A spawn that could read data owned
  outside its `taskScope` through a `view` is §3.1's open point; it needs a
  language decision, so it is its own queue task.
- The p50 of spawn → `get` is now a function call: `main` waits at once, so it
  runs the task itself.
- Sleeps (`sleeps`, `held`) are unchanged: those tasks still sleep, so they
  are threads until S3b.

**Fixtures:**

- 1073: fork-join to depth 20 (a million tasks), a merge sort that copies its
  halves, String results, and a chain of 10 000 nested waits;
- 1074: 100 000 tasks alive at once, tasks dropped unjoined, `tryGet` /
  `isDone`, a task spawning tasks, and a parallelLoop inside a task.

Both are leak-checked and on the TSan gate's list.

## 13. Resumable twins (S3b, 2026-10-09)

S3b compiles a function that may wait, and is reachable from a spawn, a
second time as a **resumable twin**: a frame struct and a step function
(`compiler/src/c_twin.c`). A spawn whose target has a twin runs as a
resumable task on the pool. Its `get()` and its sleeps suspend the task and
free the worker; a later step on any worker resumes it.

**What a twin is.**

```c
struct F__frame { int state; RaePoolSave pool; void* child; void* slots[N]; RetT result; };
static int F__step(F__frame* __rae_frame);   /* 1: finished, 0: suspended */
```

- **The normal body, re-entered.** The step function is the normal body,
  emitted by the normal statement emitter inside `switch (__rae_frame->state)
  { case 0: ... }`. There is a `case K:` label after every wait: C may jump
  into a block, past its declarations, as Duff's device does. Loops, `ret` in
  the middle of a loop, `if` / `match` and `defer` therefore need nothing of
  their own.
- **Locals saved into slots.** The locals stay C locals. At a wait, every
  local in scope (with its drop flag, and any wait result held for the same
  statement) is saved into its frame slot; after the label it is restored.
- **Slots never move.** A slot is heap storage that never moves
  (`rae_frame_slot`). So a `view` or `mod` argument to a child twin points
  into the parent's slot. It stays valid while the parent is suspended,
  which is the "borrow across a wait" the frame design promised (§4.2).
- **Waits, hoisted to the start of their statement** (in order):
  - `t.get()` on a Task named by a path: suspend until `t` is done; the
    statement then runs unchanged, on a task that is done;
  - a sleep extern (Core's `sleep`, `Time.waitUntil`'s `nativeSleepNs`):
    suspend on the scheduler's timer heap until the deadline; the call then
    reads as 0;
  - a call to another twin: a child frame is stepped until it finishes, and
    the parent suspends whenever the child does; the call reads as the
    child's result.
- **String-pool temporaries move with the frame.** A statement's own pool
  scope never spans a wait, but the function's does (an `if` condition's
  temporaries stay until the function returns). So a frame moves its pool
  entries out before it suspends and back onto the resuming thread's pool
  (`rae_string_pool_detach` / `_reattach`). Nothing touches a frame after
  its task is registered with a waker, because another worker may already
  run it.
- **Loop versioning is off in twins.** Its two copies of a loop would
  duplicate the resume labels.

**What stays as it was.**

- A wait the transform cannot reach where it stands does not suspend there:
  - inside a collection loop, a mainLoop or parallelLoop, a loop condition or
    a deferred block;
  - on the right of `and` / `or`, or in a match arm;
  - while a statement temporary is alive, or with an alias local (`=>`, a
    `view` / `mod` let) in scope;
  - a twin call with arguments that call or build things, or with a String
    result.

  There a `get()` keeps S3a's help-first wait, and a twin call runs the
  normal function. When such a wait can reach a sleep, the function is not
  a twin, because a sleeping worker would hold the pool.
- Which functions are twins is a fixed point: a function whose transform
  fails is dropped, and the twins that call it are tried again
  (`RAE_TWIN_DEBUG=1` prints the result).
- A spawn whose task can reach a sleep goes on the pool only if its target
  is a twin (`c_spawn_on_pool`). Otherwise it stays a thread, as before.
- A Task dropped at the end of its scope, and a `taskScope`, keep the
  help-first wait.
- Rae has no cancellation: a frame is only freed after it finished, by its
  own code, so "dropping a suspended frame" never happens. Dropping a Task
  whose task is suspended joins it, as for any Task (fixture 1075).
- Socket and poller waits suspend since S4 (§15); C calls that block need the `blocking`
  keyword (S5). A spawn reaching either is still a thread.

**The scheduler side** (S3a's kernel, policy in `lib/core/Scheduler.rae`):

- A task finishing hands its result over with `rae_task_complete`, which
  takes the one task registered as its waiter and wakes it.
- A sleeping task's timer sits in a locked heap. Workers move the due ones
  back to a queue (`schedulerWakeTimers`) and park no longer than the next
  timer.
- With no pool threads (`RAE_WORKERS=1`), a resumable task is driven on its
  spawner through its timers and the tasks it waits on (`schedulerDrive`,
  fixture 1076).

**Measured** (`benchmarks/spawn`, M1 Max, load 2.9; "threads" is
`results/threadPerSpawn/`, S3a the previous baseline):

| | thread per spawn | S3a | S3b |
|---|---|---|---|
| 10 000 concurrent 100 ms sleeps | 202 ms, 1.26 GB | (threads) | 128 ms, 10 MB |
| 50 000 concurrent 100 ms sleeps | 9.3 s, 2.1 GB | (threads) | 166 ms, 23 MB |
| 30 000 tasks alive at once (`held`): spawning them | 11.8 s, 2.3 GB | (threads) | 66 ms, 19 MB |
| spawn + `get`, one at a time | 22.2 µs | 3.2 µs | 1.7 µs |
| spawn + `get`, batches of 256 | 13.5 µs per task | 3.3 µs | 3.7 µs |
| spawn → `get` returned, p50 / p99 | 21 / 63 µs | 0.17 / 45 µs | 3.0 / 11 µs |
| merge sort, 1 024 / 4 096 leaves | 2.82x / 0.56x | 4.37x / 4.14x | 4.43x / 3.96x |

- **Sleeping tasks no longer need a thread each:** 50 000 sleeps take
  166 ms where they took 9.3 s, at 1% of the memory. 100 000 sleeps take
  218 ms at 41 MB, where thread per spawn stopped at 16 383 tasks.
- A chain of 100 000 tasks, each waiting on the next, suspends at every link,
  so its depth no longer depends on a worker's stack (fixture 1075).
- **A thief does not wait in line.** Workers steal with a try-lock pop, so
  the idle workers that all see a new task do not convoy on its queue's lock
  (where the spawner, taking its task back to run it, waited behind them:
  14 µs per spawn + `get` before the change). An off-pool waiter also
  watches its task for up to 50 µs before it blocks. The S3a p50 of 0.17 µs
  was the spawner winning that race every time. Now the workers win it
  sometimes, and the p99 dropped from 45 µs to 11.

**Fixtures:**

- 1075: 100 000 sleeping tasks, a chain of 100 000 waits, a wait in a loop
  with a `ret` from it, a borrow across a wait, Strings and a pool temporary
  across waits, `defer` across waits, and tasks dropped while suspended;
- 1076: the same at smaller sizes with `RAE_WORKERS=1`.

Both are leak-checked and on the TSan list, and their output is the same as
with `RAE_SPAWN_THREADS=1`. Fixture 1061 (the thread cap) builds with
`RAE_SPAWN_THREADS=1`, since its sleeping tasks would otherwise not need a
thread.

## 14. Fork-join over shared data (S3c, 2026-10-09)

S3a's measurement (§12) left two findings. Both are about fork-join.

### 14.1 The waiting thread computes too (done, 0.1.253)

A fork-join started on `main` used 7 of the 8 performance cores. The pool
has one thread per performance core but one (§12), and `main`, waiting on
its tasks, only blocked.

- **The change:** a thread off the pool now waits as a worker does. If its
  task has not started, it takes it back and runs it. If a worker has it, it
  runs other tasks from the injection queue and the workers' queues until
  its task is done, and blocks only when there is nothing to run.
- **The depth cap:** a thread off the pool runs these tasks on its own stack,
  which is the platform's (8 MB for `main`), not a pool thread's 64 MB. So
  it nests at most 32 deep (`schedulerOffPoolHelpDepth`).
- **The effect:** a recursion started on `main` has 8 compute threads,
  where it had 7.

**Measured** with `benchmarks/spawn`'s new `work` mode: a fork-join over 8 M
compute-bound items (about 90 ns each), with no data, so nothing is copied.

| leaves | 8 | 64 | 512 | 4 096 | 32 768 |
|---|---|---|---|---|---|
| speedup, 8 workers | 5.36x | 6.76x | 7.01x | 7.06x | 6.80x |
| speedup, 1 worker | 1.00x | 1.01x | 1.00x | 0.99x | 0.98x |

- **The target is met:** near-linear on 8 cores (7x), with the leaf size as
  the only cut-off. It holds from 64 to 32 768 leaves, and splitting finer
  costs nothing on one worker. With exactly 8 leaves for 8 threads there is
  no slack to balance, hence 5.4x.
- The machine was at load 4.7. The numbers are in
  `benchmarks/spawn/results/` (`work`).
- A/B in the same session, at 8 workers: with the waiting thread blocking
  (the S3b policy), the program ran 6.4x at 512 leaves and 5.2–5.5x at
  32 768; with it helping, 7.0x and 6.4–6.8x.

### 14.2 Lending a `view` to a task (done, 0.1.254)

**The problem.** Fork-join over data was bound by copying, not by tasks:

- The 16 M sum stayed at 0.06x of sequential on the pool, because one copy
  of the input costs more than the whole sequential sum (§10, §12).
- The same sum through `parallelLoop`, whose iterations read the shared list
  in place, is faster than sequential.

A spawn copies (or moves) every argument. A `view` argument of a list or
struct was rejected (docs/concurrency-model.md), because nothing guaranteed
that the borrowed place outlives the task or stays unchanged while it runs.

**The rule (approved by the maintainer 2026-10-10).** Inside a `taskScope
{ }`, a `spawn` may pass a `view` of a place that outlives the scope:

1. **What may be lent.** A local declared before the scope, a parameter of
   the enclosing function (a `view` parameter's own referent outlives the
   call), a path into one of those (`a.b`, `a[i]`), or a string literal.
   Never a value made inside the scope: that is the old error, with a hint.
2. **The lent place is frozen until the scope ends.** From the spawn to the
   end of the scope, the compiler rejects every write to the lent local:
   assignment, a mutating method, a `mod` argument, a move (`own`). The
   error names the spawn's line.
   - This is the frozen-`let` check (docs/let-is-frozen.md), applied to a
     `var` for the rest of the scope; a `let` is frozen already.
   - Reads stay allowed, in the scope and in the tasks: they are all reads,
     so there is no race.
3. **The scope waits for its borrowers.** Each taskScope that lends keeps a
   task group (`RaeTaskGroup`, a count). A lending spawn adds one to it, its
   task subtracts one as its last act, and the scope waits for zero at its
   end and before a `ret`, `break` or `continue` leaves it. The waiting
   thread helps run tasks meanwhile, as `get` does (§14.1).
4. **Only `view`.** A `mod` argument of non-scalar data is still rejected:
   two tasks writing the same place would race. Writing disjoint parts in
   parallel is `parallelLoop`'s job, with its own rules.
5. **Outside a `taskScope` nothing is lent**, not even a `let` (the question
   (b) below): with no scope, nothing bounds where the task's borrow ends.

**One change from the proposal.** The proposal bounded the borrow by
forbidding a lending Task to leave the scope (no return, no container). The
group wait (3) bounds it instead: the scope waits for every task that
borrows from it, wherever its handle went, so the escape check is not
needed. It is just as safe and allows the common form, Tasks collected in a
`List(Task(T))` and joined in a loop. A handle that does outlive the scope
holds a finished task, and its `get()` returns at once.

The sketch of §3.1 is now written:

```rae
func parallelSum(numbers: view List(Int), start: copy Int, count: copy Int) ret Int {
  if count < 4096 {
    ret sequentialSum(numbers: numbers, start: start, count: count)
  }
  let half: Int = count / 2
  var total: Int = 0
  taskScope {
    let left: Task(Int) = spawn parallelSum(numbers: numbers, start: start, count: half)
    let right: Int = parallelSum(numbers: numbers, start: start + half, count: count - half)
    total = left.get() + right
  }
  ret total
}
```

**How it is built.**

- **Sema** (`sema_spawn_lends`): a stack of the open taskScopes and the
  scope depth each opened at. A spawn lends when each non-scalar `view`
  argument roots in a symbol declared at or above that depth. It marks the
  symbol `lent_line`, which `expr_roots_in_let` treats like a `let` until the
  scope pops.
- **Codegen** (`c_lend.c`): a lent view is the pointer a normal call passes.
  Each lent-to function gets an argument pack whose fields have the
  parameters' own C types. The spawn site emits the normal call's text with
  the function swapped for the pack's constructor, so every argument is
  lowered exactly as for a call. The task runs on the pool when it may
  (`may_wait_spawn_on_pool`), otherwise on a thread. A resumable twin (§13)
  does not suspend inside a lending scope: tasks borrow its C locals.
- Fixtures: `1077_spawn_lend_view` (fork-join, `ret`/`break`/`continue`
  out of the scope, a lent String, a task that lends in turn; also under
  TSan) and `1078_reject_spawn_lend`.

**Measured** with `benchmarks/spawn`'s `sum` mode (new metric
`lentForkJoinMs`): 16 M integers, 8 workers, load 4.5, median of 5.

| leaves | 1 | 8 | 64 | 1 024 | 4 096 |
|---|---|---|---|---|---|
| sequential | 2.3 ms | 2.6 ms | 2.9 ms | 2.6 ms | 2.7 ms |
| fork-join, copying | 12.5 ms | 31.1 ms | 37.6 ms | 40.1 ms | 46.3 ms |
| fork-join, lent | 2.3 ms | 1.4 ms | 1.5 ms | 1.9 ms | 1.9 ms |
| `parallelLoop` | 1.4 ms | 1.4 ms | 1.4 ms | 1.4 ms | 1.4 ms |

- **The copy is gone.** Lending is 20–25x faster than copying and matches
  `parallelLoop`.
- This sum is bound by memory bandwidth (one add per 8 bytes), so neither
  form gets far past 2x of sequential. For a compute-bound fork-join, see
  the 7x of §14.1.

**Alternatives that were weighed.**

- Keep copying: fork-join over data stays slower than one core.
- A shared immutable value type (reference-counted, like Rust's `Arc`): a
  new type and runtime machinery, and data has to be put into it first.
- A parallel-calls construct (F9): new syntax, and it still needs the same
  freeze to share data.

## 15. Socket waits suspend (S4, 2026-10-10)

A task waiting on a socket now suspends where it held its worker. The waits
are lib/net's two: `systemPollOne` (one socket, used by
`socketWaitReadable` and by `tcpConnect` while a connection is in progress)
and `systemPollerWait` (a whole poller, `pollWait`).

**In a twin** (`c_twin.c`), such a call is hoisted like a sleep:

1. Its arguments are evaluated once and held in the frame (`hold[4]`).
   They may be calls (`netConstant(which: pollReadable)`) when the wait is
   the statement's whole value, so nothing else of the statement is moved.
2. The task registers its interest and suspends (`rae_sched_io_wait`).
3. Resumed, the call runs with its held arguments and a timeout of 0. It
   answers what is ready, or 0 when the wait timed out, so the code after it
   needs no change.

When the task cannot suspend (a timeout of 0, a platform without kqueue),
the call runs with its own timeout, as in the normal function.

**In the runtime** (`runtime_sched_io.c`), the scheduler owns one kqueue.

- **A wait** is two one-shot registrations carrying the task: the socket's
  read or write filter, and a timer when there is a timeout. A poller wait
  watches the poller's own descriptor, which is readable when it has events.
- **The readiness thread** runs `schedulerIoLoop` (Scheduler.rae). It is
  started by the first task that waits. It takes one ready task at a time,
  and puts it on the injection queue as `schedulerWake` does for any woken
  task.
- **No double wake.** Taking a task deletes its other registration before
  the task is queued. A lock orders a registration against the taking of its
  task, and an `io_armed` flag tells a live wait from one whose registration
  failed. So a socket and its timeout firing together wake the task once.
- **One task per socket direction.** kqueue keys an interest by descriptor
  and filter, so two tasks waiting to read the same socket would replace
  each other's interest. Reading one socket from two tasks is a bug anyway.

**What a pool task may now reach** (`may_wait.c`):

- lib/net's system calls, which are non-blocking (a socket answers
  `wouldBlock`);
- lib/String's externs.
- Not the name lookup (`systemResolve`, getaddrinfo): a DNS query can wait
  for seconds, so a task that resolves a name (`tcpConnect`) stays a thread
  until the `blocking` keyword (S5) runs that call on its own thread.

**Measured** with fixture `1080_spawn_socket_waits` on two workers:

- Main spawns 64 handler tasks, oldest first. Each waits for its client, and
  main asks the newest client first.
- With socket waits that held the worker, the two oldest handlers held both
  workers until their 2 s timeout: 1 of 64 answered, in 44 s.
- With suspending waits, 64 of 64 answered in 2 s, including the build.

**Not yet:**

- **Linux:** its readiness poller (epoll) is not written yet. There
  `RAE_SCHED_IO_SUSPENDS` is 0, and a spawn that can reach a socket wait
  stays a thread, as before S4 (the spawn site tests the constant, which
  the C compiler folds away).
- **The browser:** tasks interleaving with `mainLoop`'s frames.

