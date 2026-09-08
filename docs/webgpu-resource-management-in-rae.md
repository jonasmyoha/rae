# WebGPU resource management in Rae

Status: **historical task #295 plan reconciled with the revision 2 proposal;
new ownership and entry-point semantics await maintainer approval.**

The active proposal is [Ptr and GPU resources](ptr-and-gpu-resource-design.md).
Its [implementation contract](ptr-and-gpu-resource-design.md#implementation-contract)
replaces this document's former raw-handle sketches and ownership rules. Earlier
text and old module paths remain available in Git history rather than presenting
a second implementation plan alongside the current one.

## Division of responsibility

Rae owns resource descriptors, validation, caches, labels, resource groups,
rebuild decisions, command dependencies and completion scheduling. Native storage
is per context and holds actual platform objects. Generated bindings perform
WebGPU operations; narrow generic ABI glue handles opaque representations and
stable asynchronous callback state. Keep the renderer C-surface gate in force.

`GpuResources` is an App/World-owned resource containing a registered native
context owner and Rae metadata. Public operations receive that resource explicitly
through `view` or `mod`. No hidden current-manager getter or global slot table
may substitute for the parameter. Two instances must be independently usable.

## Public resource values

Use distinct `TextureId`, `TextureViewId`, `BufferId`, `SamplerId`, `PipelineId`,
`BindGroupId`, `ReadbackId` and `SubmissionId` values. These are non-owning, copyable
identities, not wrappers that auto-release a resource when copied values leave
scope. Byte ranges pair a buffer ID with checked offsets and sizes.

The manager validates owner identity, kind, slot, generation, live state and
operation-specific requirements before native access. Releasing a resource
invalidates new uses immediately, then defers physical cleanup until recorded,
submitted and callback dependencies permit it. Never recycle an exhausted
identity or generation counter.

For cross-App context identity, revision 2 proposes a launcher-owned runtime root
passed through optional `main(runtime: mod RuntimeResources)`. That is an explicit
pending language decision, not an existing entry-point feature or permission to
introduce a global identity counter.

## Resource caches and dependencies

Rae descriptors and cache keys remain ordinary deeply copied data. A cache entry
contains an ID into its manager, not an independently owning native handle.
Explicit replacement retires the old resource and invalidates dependent bindings;
dropping an arbitrary copy of cache metadata must not release shared resources.
The manager is the final owner and cleanup backstop.

A command recording retains everything it uses, including resources referenced
through bind groups and views. Unsubmitted abandoned recordings release their
holds; submitted work associates them with completion. The earlier rule of
releasing once callers stopped recording was insufficient for asynchronous GPU
execution and is withdrawn.

Readback and timing use bounded staging and nonblocking completion polling.
A mapping callback must never retain a pointer into a movable Rae owner or
growable list. Shutdown cancels/drains or transfers native callback ownership
safely; a timeout cannot justify freeing reachable callback state.

## Migration scope

The first complete consumer is water: replace numeric global C slots with named
IDs and instance groups while preserving wave timing, FFT tiers and cache
semantics. Then migrate shared 3D, grass/sprites, 2D/UI and remaining compute
consumers in queue dependency order. Existing maintained examples must remain
buildable between stages; explicit adapters may not create new singletons.

Do not combine this ownership migration with a new renderer, bindless API,
backend replacement, unrelated batching rewrite or render-graph optimization.
Checked draw/dispatch packing and existing WebGPU binding layouts are sufficient
for this scope. Later render-graph changes require their own task and measurements.

## Acceptance

Use the replacement contract's compiler and native-lifetime tests, independently
owned instances, before/after CPU/GPU measurements, and affected Compiled example
and screenshot gates. Run suites through `compiler/tools/watch-tests.sh` with
explicit timeouts; use timeouts for hardware checks too. Approving this plan does
not mean that these tests have already passed or the migration is implemented.
