# WebGPU resource management in Rae

Status: **revision 3 direction accepted; language details await example-led review.**

The active proposal is [Ptr and GPU resources](ptr-and-gpu-resource-design.md).
Historical task #295 sketches and the rejected revision 2 registration/runtime
injection proposal are not implementation instructions. Keep normal `func main()`
and make the general low-level facility available to all library authors.

## Ownership and API

`GpuResources` is an explicitly passed App/World resource. It owns per-context
native storage and Rae metadata. Public `TextureId`, `TextureViewId`, `BufferId`,
`SamplerId`, `PipelineId`, `BindGroupId`, `ReadbackId` and `SubmissionId` values
are non-owning identities. Copying them does not create or release GPU resources.
Copying the manager must fail unless a real independent copy is possible.

Rae owns descriptors, validation, caches, labels, resource groups, rebuild policy,
command dependencies and completion scheduling. Native calls use generated
bindings and narrow generic ABI glue for opaque objects and stable callback state.
Safe wrappers may use explicitly unsafe implementation operations; no compiler
list of privileged module names grants access. The exact source-level native
owner and unsafe-function/block contracts are still pending queue #878 review.

## Resource validity and completion

Before native access, validate owner identity, kind, slot, generation, live state,
usage and overflow-safe ranges. Cross-App identity must work with ordinary context
constructors; no injected runtime parameter or hidden current-manager API is
approved. Generation exhaustion must not make stale IDs valid again.

The manager holds dependencies from command recording through submission and
completion, including indirect bind-group/view dependencies. Abandoned recordings
release holds. Retiring a resource rejects new uses and defers physical cleanup
until existing uses finish. Merely returning from a CPU borrow is insufficient.

Readback and timing use bounded staging and nonblocking normal polling. Callback
state must survive cancellation without retaining pointers into movable Rae data.
Explicit shutdown may drain or safely transfer native completion state; timeouts
must not force release of callback-reachable memory. Keep blocking diagnostics
available separately.

## Migration and acceptance

After the general owner/low-level example is approved and implemented, build the
manager and completion API, then migrate water and the other renderer consumers.
Replace numeric water C slots with named typed IDs and independent instance
groups. Dropping copied cache/controller metadata does not automatically release
its manager-owned resources. Keep adapters explicitly owned during migration.

Do not combine this work with a backend replacement, new water features,
bindless API, batching rewrite or render-graph optimization. Centralize checked
Rae/WGSL argument packing, retain the C-surface gate and keep maintained examples
buildable between stages.

Verify owner copy/drop rules, stale/wrong-context IDs, two independent instances,
resize/rebuild/shutdown, late cancellation callbacks, matched CPU/GPU measurements
and affected Compiled screenshot gates. Run suites through
`compiler/tools/watch-tests.sh` with explicit timeouts, and time-bound hardware
checks. These requirements are not claims of already implemented guarantees.
