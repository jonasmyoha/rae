# Lightweight `spawn`: design

**Status:** design, queued 2026-10-08 as five tasks (S1–S5, following §8).
Nothing is implemented. The tasks take §9's recommendations as working
decisions unless the maintainer changes a fork here. S1 measures first and
says whether the numbers justify continuing (F8).

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
  let left: Task(Int) = spawn sumRange(start: start, count: half)
  let right: Int = parallelSum(numbers: numbers, start: start + half, count: count - half)
  ret left.get() + right
}
```

(A sketch: in real code the spawned half takes its slice by `copy` or `own`,
since a `spawn`'s arguments cannot borrow from the caller. Making the halves
share read-only data without copying is part of what S3 measures.)

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
   which functions would become resumable, and how many.
3. Resumable codegen for `Task.get` and `sleep` only, on the existing worker
   pool, with fixtures (a deep chain of waits, a wait inside a loop, a borrow
   across a wait, dropping a suspended task, leak-checked; TSan).
4. Socket and poller waits (after G2) and the browser path.
5. The extern-blocking and fairness choices (F2, F3).

## 9. Decisions for the maintainer

1. **F1 Implementation:** stackless inferred state machines (recommended:
   portable C, works in wasm, fits the whole-program compiler) or stackful
   tasks.
2. **F2 Blocking externs:** a bare modifier keyword on such externs
   (recommended, explicit and simple), Go-style hand-off, or nothing.
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
8. **F8 When:** after the server phase 1 numbers exist (recommended), or now.
9. **F9 A parallel-calls construct:** later, and only if `taskScope` +
   `spawn` reads too heavily for fork-join once spawn is cheap: a construct
   that runs independent calls in parallel, with independence proven from
   parameter modes (§3.1). It is new syntax, so it needs approval. Not now
   (recommended).
