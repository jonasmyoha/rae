# Staged parallel work for the physics solver — design

Status: **design, awaiting the maintainer's choice** (2026-10-03). Nothing in
this document is implemented. It answers `docs/physics-performance-plan.md`
§5 item 3: how Rae runs Box3D's solver — a few hundred dependent stages per
step — on the worker pool, and with which language surface. The worker pool
and `parallelLoop` it builds on have landed (`docs/concurrency-model.md` §5).
Until a design lands the solver is sequential and the `parallelLoop` rules
stay as they are. Per-worker scratch is out of scope: it is chunk scratch
(fixture 946), and no worker-index rule is added.

## 1. What the solver does

Box3D's `b3SolverTask` (`solver.c`) runs, per step:

```
prepare joints · prepare wide contacts · prepare contacts
per substep (4):
  integrate velocities
  warm start            — one stage per graph colour
  solve                 — one stage per graph colour
  integrate positions
  relax                 — one stage per graph colour
restitution             — one stage per graph colour
store wide impulses · store impulses
```

With 12-24 active colours that is **about 220 stages per step**, each of which
must finish before the next starts. Within a stage the work is independent:
a colour's constraints touch disjoint dynamic bodies — that is what the
colouring is for — and the body stages touch each body once.

Box3D keeps every worker inside one task for the whole step. The calling
thread is the leader (`b3ExecuteMainStage`). It publishes each stage in one
atomic word and executes blocks itself. Helpers spin until a stage appears,
claim blocks with a compare-and-swap on each block's monotonic sync index,
starting from an offset derived from their worker index, and add to the
stage's completion counter. The leader spins until the counter is full. A
one-block stage runs on the leader without publishing at all. The leader
alone can finish every stage, so a missing or late helper never deadlocks
the step — which is how Box3D runs with any task system, a serial one
included.

## 2. What a stage barrier costs today

`benchmarks/parallel_stages/run.sh` runs a solver-shaped step: 220 stages of
32 blocks, ~1.5 µs of arithmetic per block (about the pyramid's
single-threaded solver budget). It runs it two ways. *Staged* is 220
back-to-back `parallelLoop`s, one per stage. *Flat* is the same 7 040 blocks
as one `parallelLoop`, with no barriers. The barrier cost is the difference
divided by 220. Measured on the M1 Max at load ~5:

| workers | staged | flat | barrier per stage | staged speedup |
|---|---|---|---|---|
| 1 | 10.43 ms | 10.51 ms | — | 1.00x |
| 2 | 5.65 ms | 5.36 ms | 1.3 µs | 1.85x |
| 4 | 3.08 ms | 2.77 ms | 1.4 µs | 3.38x |
| 8 | 2.41 ms | 1.38 ms | 4.7 µs | 4.33x |

So the premise that re-dispatching a pool job per stage would cost more than
the stage does not hold. The pool's workers are still spinning between
back-to-back launches, so a launch is a publication and a join, not a thread
wake-up.

- At **4 workers** a stage-per-`parallelLoop` step costs about 10% over the
  barrier-free ideal.
- At **8 workers** it costs about 75%. All of that is the launch path: one
  shared claim word that 8 workers contend on, and a join that waits for the
  slowest.

For context, Box3D's own C build reaches only **3.4x at 8 workers** on the
5 050-box pyramid (`docs/physics-performance-plan.md` §2). Its spin barriers
are not free either.

## 3. The problem every option has: coloured writes

A solve stage writes each constraint's two bodies:

```rae
states.set(index: constraint.bodyA, value: newA)
```

The index comes from data, not from the loop variable. The `parallelLoop`
checker rejects that write, correctly: it cannot know that the graph
colouring keeps two constraints of one colour off the same body. Nor can the
algorithm be restructured to write only own-index elements. Gathering the
impulses per constraint and scattering them in a later stage turns Box3D's
Gauss-Seidel order into a Jacobi-like one, so the bits differ from the
oracle, and the port's whole verification (P1-P6, bit-exact) is lost.

So whichever option is chosen, the language needs a way to say "these writes
are disjoint, by an argument the compiler cannot check". The proposal reuses
the existing keyword rather than adding one:

- **`unsafe { }` inside a `parallelLoop` body lifts the write rule for the
  statements in it.** `unsafe` already means "the programmer vouches for what
  the compiler cannot check" (extern calls). Here it vouches that no two
  iterations of the loop write the same element, and that no iteration reads
  an element another one writes. It is visible, greppable and local: the
  colouring argument goes in a comment beside it.
- **A runtime check backs it up.** With `RAE_PARALLEL_CHECK=1`, the runtime
  records which iteration wrote each (buffer, index) through an `unsafe`
  write inside a parallel body, and aborts on a second writer:
  `parallelLoop: iterations 41 and 97 both wrote states[17]`. It is off by
  default (it costs a table insert per write). The port's fixtures run with
  it on, alongside the TSan gate and the oracle's bit-exact comparisons.

Everything else in the stage — the constraint itself, its impulses — is the
element of the iteration's own index. Today's rule already covers that.

## 4. Option C — a stage is a `parallelLoop` (recommended)

No new construct. The solver's stage loop is an ordinary `loop` whose stages
are `parallelLoop`s over the constraints (or bodies) of the stage. The pool
cuts each range into chunks, which is what Box3D's blocks are.

### Worked example: one colour's solve stage

```rae
# A graph colour's contact constraints are solved in parallel. The colouring
# guarantees no two constraints of one colour share a dynamic body, so the
# body writes below are disjoint (RAE_PARALLEL_CHECK=1 verifies it in the
# fixtures).
func solveColour(
  colour: view GraphColour
  constraints: mod List(ContactConstraint)
  states: mod List(BodyState)
  context: view StepContext
) {
  parallelLoop var k: Int = colour.first, k < colour.end, ++k {
    if let constraint: mod ContactConstraint => constraints.modAt(index: k) {
      let a: BodyState = states.copyAtDefault(index: constraint.bodyA)
      let b: BodyState = states.copyAtDefault(index: constraint.bodyB)
      let solved: ContactSolve = solveContact(constraint: constraint, a: a, b: b, context: context)
      constraint.normalImpulse = solved.normalImpulse   # own element: allowed
      unsafe {
        states.set(index: constraint.bodyA, value: solved.a)
        states.set(index: constraint.bodyB, value: solved.b)
      }
    }
  }
}

# The step's stage loop — Box3D's b3SolverTask without any synchronisation code:
loop var substep: Int = 0, substep < context.substepCount, ++substep {
  integrateVelocities(states: states, sims: sims, context: context)    # a parallelLoop over bodies
  loop var colour: Int = 0, colour < graph.activeColourCount, ++colour {
    warmStartColour(colour: graph.colours.viewAt(index: colour), ...)
  }
  loop var colour: Int = 0, colour < graph.activeColourCount, ++colour {
    solveColour(colour: graph.colours.viewAt(index: colour), ...)
  }
  integratePositions(states: states, context: context)
  loop var colour: Int = 0, colour < graph.activeColourCount, ++colour {
    relaxColour(colour: graph.colours.viewAt(index: colour), ...)
  }
}
```

(A real port splits each colour into joint and contact ranges and iterates
the wide constraints in groups of four, as Box3D does. The shape is the
same.)

### Determinism

Identical bits for any worker count, by construction:

- each stage finishes before the next starts (the `parallelLoop` join);
- within a stage, iterations touch disjoint data (own index, plus the
  coloured writes vouched for in §3);
- nothing is counted or claimed by user code.

The one thing not checked at compile time — the colouring — is checked at run
time in the fixtures (`RAE_PARALLEL_CHECK=1`), and against the oracle at
`workerCount = 1`.

### RAE_WORKERS=1

Every `parallelLoop` runs inline on the caller, so the stages run in order on
one thread. There are no spin loops in Rae code, so nothing can deadlock.

### Work

- **Compiler:**
  - `unsafe` inside a `parallelLoop` body lifts the write rule for its
    statements (sema; ~30 lines);
  - with `RAE_PARALLEL_CHECK` builds, writes through `set` / `modAt` inside
    such a block call a checked runtime variant;
  - a fixture with a correct and a deliberately wrong colouring.
- **Runtime:**
  - make back-to-back launches cheap at 8 workers, where Box3D's own design
    shows the way:
    - each worker starts claiming from its own offset (affinity across
      stages);
    - claim per chunk rather than through one shared word;
    - a stage with one chunk runs inline without publishing;
    - per-worker completion flags instead of one shared counter;
  - the `RAE_PARALLEL_CHECK` conflict table.
  - **Acceptance:** `benchmarks/parallel_stages` at 8 workers within ~25% of
    flat — barrier ≤ ~1.5 µs per stage — and no regression at 2 and 4.
- **New surface:** none, beyond the meaning of `unsafe` in a parallel body.

## 5. Option A — atomics, a CPU relax, and a team construct

A faithful transliteration of Box3D: the solver is one parallel region in
which every worker runs Box3D's own loop. The leader publishes stages, and
helpers spin, claim blocks with compare-and-swap and count completions.

**Surface:**

- `Atomic(Int)` and `Atomic(Int32)`, library types over C11 `stdatomic`:
  `load`, `store`, `fetchAdd`, `compareExchange`, all sequentially consistent.
  No memory-order parameters, and never on floats. Their methods take
  `this: view Atomic(T)`, so an atomic in shared data is usable from a
  parallel body without a new write rule: an atomic operation is race-free by
  definition.
- `Parallel.cpuRelax()`: a library function (`yield` / `pause`).
- **A team construct** — the "guaranteed-concurrent mode". A library function
  cannot take a body (Rae has no function values), so it needs a keyword,
  for example:

  ```rae
  parallelTeam var worker: Int {
    if worker is 0 {
      # leader: publish each stage, run blocks, wait for the stage's count
    } else {
      # helper: spin on the published stage, claim blocks, add to the count
    }
  }
  ```

  It runs its body once per worker, at the same time, with `worker` from 0 to
  `workerCount - 1`.

**Determinism.** Identical bits for any worker count *only if* the program
keeps three rules the compiler cannot check:

1. blocks within a stage are independent (the colouring);
2. atomics only claim and count work, never accumulate results;
3. which blocks a worker happens to claim changes nothing it writes.

The `unsafe` coloured writes of §3 are needed here too.

**RAE_WORKERS=1, and progress in general.** The team construct cannot promise
that all its iterations are live at once. With one worker it can't, and a
descheduled helper is the same case in miniature. So the rule must be Box3D's:

- iteration 0 always runs, on the calling thread, and alone can finish every
  stage;
- helpers only steal;
- nothing ever waits for a specific helper.

With `RAE_WORKERS=1` the team is the leader alone, and the stages run in order
on one thread. But the rule is a convention, not a checked property. A
leader written to wait for "every helper has arrived" deadlocks at one
worker, and hangs at any count whenever a helper is late. Rae would hand
every program the ability to write a spin barrier that deadlocks.

**Work:**
- `Atomic(T)` in `lib/` plus inline runtime functions;
- `cpuRelax`;
- the `parallelTeam` keyword: parser, formatter, sema, and lowering to a pool
  launch of `workerCount` iterations, one per worker, with iteration 0 on the
  caller;
- checker rules for what a team body may write;
- documentation of the progress rule;
- fixtures, including a deliberately wrong barrier that must be caught by a
  test timeout rather than hang the suite.

The largest surface of the three, and the only one that can deadlock.

## 6. Option B — a stage construct that hides the atomics

A dedicated construct runs a sequence of stages with a barrier between each.
The runtime does the publishing, claiming and joining, Box3D-style, with
workers resident across the whole sequence:

```rae
parallelStages var stage: Int = 0, stage < stageCount, ++stage {
  # the stage's work: a range the runtime chunks
  stageRange(stage) as var k: Int {
    ...
  }
}
```

Its semantics are exactly Option C's — a sequence of parallel loops with a
join after each — so its determinism and `RAE_WORKERS=1` story are the same,
with no deadlock possible. What it adds is that the runtime sees the whole
sequence up front and can keep the workers in one region instead of
re-launching. Its cost is a second loop construct with nested ranges,
parsing and checking rules, and formatter support. It wins over Option C only
if the runtime cannot make back-to-back `parallelLoop` launches cheap enough
— and C's acceptance benchmark answers that before any syntax is written.

## 7. Comparison

| | C: stage = parallelLoop | A: atomics + team | B: stage construct |
|---|---|---|---|
| new surface | `unsafe` meaning in a parallel body | `Atomic(T)`, `cpuRelax`, `parallelTeam` | `parallelStages` + nested ranges |
| determinism | by construction (+ checked colouring) | by convention (3 rules) | by construction (+ checked colouring) |
| RAE_WORKERS=1 | sequential stages | sequential if the progress rule holds | sequential stages |
| can deadlock | no | yes, on a wrong barrier | no |
| barrier cost today | 1.4 µs (4 w), 4.7 µs (8 w) | Box3D-level, ~1 µs (not measured in Rae) | Box3D-level, if built |
| closeness to Box3D's C | the stage list is the same; the sync code disappears | line by line | the stage list is the same |
| atomics in the language | not needed for the solver | yes | no |

Box3D's other atomics need none of them either. The parallel appends
(`bulletBodyCount`), the `anyRestitution` flag and the per-worker bitsets
become chunk scratch: per-chunk lists and flags merged in chunk order
(fixture 946).

## 8. Recommendation

**Option C.**

- **Surface:** it adds no construct, and the only change to the language's
  meaning is `unsafe` lifting the write rule in a parallel body, which every
  option needs anyway.
- **Correctness:** it keeps determinism by construction, and it cannot
  deadlock.
- **Remaining work:** a runtime launch-path improvement, measured by an
  existing benchmark.
- **Escalation:** if that improvement cannot bring 8-worker barriers near
  1.5 µs, Option B is the next step, and its semantics are already C's.
- **Option A:** not recommended. Its speed is the same as a well-implemented
  B, and it gives every Rae program unchecked atomics and deadlocking
  barriers.
