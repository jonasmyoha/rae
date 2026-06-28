# Clo memory

## Durable Rae design principles (carry across tasks)

- **Value/ref boundary:** `mod`/`view`/`own`/`=>` make refs; else by value.
- **Two backends, one language:** Compiled (C)=perf/parallel; Live (VM)=
  iteration/tooling/hot-reload; same semantics required. VM has shared
  interpreter state + global `g_mem_*` counters (race hazard).
- **Ownership model is already a borrow checker** (view/mod/own/copy +
  cascade-drop, no GC) — reuse it, don't add traits.
- Cross-module global writes limited; pass structs in, don't mutate globals.
- `sys_thread.c/.h` = cross-platform thread+mutex (pthread/Win32). malloc, no GC.

## Roundtable: Concurrency design (rounds 1–2). My position.

Thesis: concurrency rides on the ownership system — view/mod/own already
encode Send/Sync; compiler proves race-freedom at the spawn boundary.

**Verified ground truth (r2):** `spawn` ALREADY EXISTS but is a half-built
prototype: vm.c `OP_SPAWN`/`SpawnData`/`spawn_thread_wrapper` spawns a
DETACHED OS thread running a FRESH sub-VM, moves args in, NEVER joins (leaks
handle, no result, no await). **VM-only — NO C-backend codegen.**
`docs/multiplayer-highscore-plan.md` commits to "threads-over-async, spawn
model, func..spawn{}". So: evolve an isolate prototype, not greenfield.

**Design:**
- `spawn <expr>` → `Task(T)` (change detach→joinable handle). **Await UNMARKED**
  (read Task→auto-join) — held user's line vs Chattie/Gem's `.wait()/.get()`
  (those = explicit escape hatches: poll()->opt T, select).
- **Structured vs detached (central rule, adopts Gem's insight):** scoped tasks
  (`taskScope`/`TaskGroup`, `parallelFor`) join before scope return → join
  barrier lets them BORROW outer view/mod. Detached (Task escapes) → `own`
  (move) only. Ownership=safety; structured-vs-detached=borrow-allowed.
- **Two strata:** (1) independent tasks = isolate/move — Live sub-VM made
  joinable+result-returning; C = thread-pool + moved args + Task struct
  (slot/done-flag/condvar), NET-NEW. (2) Data-parallel ECS = shared-memory
  `parallelFor` over disjoint `mod` shards — Compiled real threads, Live
  sequential fallback (same result). Isolates CANNOT share a table view, so
  ECS parallelism MUST use parallelFor not spawn — key tension.
- Stdlib: Task(T), Channel(T) (send(own)/recv), Shared(T) (mutex), Atomic.
- raylib main-thread-only (unanimous). No conventional async/await.

**Steps:** make existing spawn joinable → C-backend spawn + parallelFor →
frontend (Task/taskScope/Channel/Shared/Atomic) → borrow rules (scoped=borrow,
detached=own) → audit g_mem_*/interned pools for thread-safety → both-backend
+ Live-vs-Compiled equivalence tests. Doc: `rae/docs/concurrency-model.md`.

**Risks:** (must-pin-first) does vm.c arg "transfer" deep-copy or pointer-move?
— decides if isolate model is safe today. g_mem_*/interned pools raced across
sub-VMs on OS threads. Task drop/lifetime ties to scope-exit-dealloc. Hot-
reload w/ live tasks. Soundness of scoped borrow rules (no traits).

NOTE: peers' r1 replies were ~half contaminated by a PRIOR roundtable (Live VM
view/mod mobile-UI fix, OP_INIT_LOCAL, viewport sim) — off-topic for concurrency.

(Solo context: busy-render loop replaced event-wait; ECS O(1) sparse set.)
