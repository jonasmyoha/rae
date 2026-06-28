- 2026-03/05 durable compiler context:
  - C backend recovery guardrail: no `RaeAny` shortcut for generic `T`; cloned generic bodies must not reuse stale `decl_link`/resolved-type state.
  - Mobile UI Live failure root cause was narrowed to VM value/ref lowering: `Screen` enum auto-viewing plus `OP_SET_LOCAL` used for `let` initialization. Preferred fix: enum-aware `vm_is_value_type`, then direct local init via `OP_BIND_LOCAL`/`OP_INIT_LOCAL`.

- 2026-06-23 concurrency design:
  - Rae should use spawn-first structured concurrency: normal calls are synchronous; `spawn` returns `Task(T)`; result retrieval is explicit via `task.get()`/`join`, with no `await` keyword.
  - Core model: `Task(T)`, `TaskGroup`, `Channel(T)`, `CancellationToken`; OS threads/thread pool for Compiled, isolated per-task VM contexts for Live unless/until VM globals are thread-safe.
  - Round 2 refinement after peers: lean on Rae ownership modes (`view`/`mod`/`own`/`copy`) for spawn-boundary safety; treat `TaskGroup` as default, detached tasks as explicit.
  - Safety rules: spawned work cannot capture unsafe stack refs; shared `mod` needs ownership or disjoint shards; Raylib/render calls stay main-thread only.
  - ECS/data parallelism should use explicit `parallelFor`/component-table sharding; structural sparse-set mutation remains single-threaded unless separately designed.
