- 2026-03/05 durable compiler context:
  - C backend recovery guardrail: no `RaeAny` shortcut for generic `T`; cloned generic bodies must not reuse stale `decl_link`/resolved-type state.
  - Mobile UI Live failure root cause was narrowed to VM value/ref lowering: `Screen` enum auto-viewing plus `OP_SET_LOCAL` used for `let` initialization. Preferred fix: enum-aware `vm_is_value_type`, then direct local init via `OP_BIND_LOCAL`/`OP_INIT_LOCAL`.

- 2026-06-23 concurrency design:
  - Rae should use spawn-first structured concurrency: normal calls are synchronous; `spawn` returns `Task(T)`; result retrieval is explicit via `task.get()`/`join`, with no `await` keyword.
  - Core model: `Task(T)`, `TaskGroup`, `Channel(T)`, `CancellationToken`; OS threads/thread pool for Compiled, isolated per-task VM contexts for Live unless/until VM globals are thread-safe.
  - Round 2 refinement after peers: lean on Rae ownership modes (`view`/`mod`/`own`/`copy`) for spawn-boundary safety; treat `TaskGroup` as default, detached tasks as explicit.
  - Safety rules: spawned work cannot capture unsafe stack refs; shared `mod` needs ownership or disjoint shards; Raylib/render calls stay main-thread only.
  - ECS/data parallelism should use explicit `parallelFor`/component-table sharding; structural sparse-set mutation remains single-threaded unless separately designed.

- 2026-07-24 3D renderer evaluation:
  - Scores: 107 raymarch 6.8/10; 108 procedural raymarch 7.4/10; 109 PBR raster 7.9/10. 109 is the best engine base; 108 is the better raymarch pass; retain 107's split static camera/object/material buffers.
  - 107 strengths: split GPU buffers, 3D FBM/animated SDF, soft shadows/AO. Weaknesses: approximate PBR, CPU GPU-readback/present, positional parallel arrays, temporal jitter without accumulation.
  - 108 strengths: entity-linked transforms, Vec3 material model, GGX/Smith/Schlick, torus, 2x2 supersampling. Weaknesses: repacks/uploads full scene each frame, monolithic buffer, same synchronous readback path.
  - 109 strengths: real generated meshes, direct raster pass, depth, 4x MSAA, GGX, ACES, Rae math/mesh modules. Weaknesses: raw Int handles, per-frame heap Mat4 lists, global/fixed C registries, embedded WGSL, gpu2d-global coupling, LDR target, no shadows/textures/SSAO, Y-up conflicts with documented Z-up.
  - Unify the front end, not geometry representation: `Scene3d` ECS + O(n) extraction + shared material/light/camera/environment/post data; raster `MeshRenderer` and separate `SdfVolume`/`RaymarchPass` feed one HDR render graph.
  - Public API should use typed handles/resources and value `Mat4`/`Quat`, with Rae owning renderer/resource policy; C remains thin WebGPU/SDL ABI. External WGSL, explicit resource destruction/device-loss handling.
  - Migration order: reconcile queue/docs; add math/handle/scene tests; build scene3d ECS; refactor 109 onto it; remove shader/policy from C; add HDR/tonemap; port 108 as GPU-texture raymarch node without CPU readback; then shadows/SSAO/post.
