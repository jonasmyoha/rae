# Ptr and GPU resources in Rae

Status: **proposal for maintainer review, 2026-09-09; not implemented or approved language semantics.**

## Recommended direction

Keep ordinary Rae programming free of raw pointers. Use typed, copyable resource
identities such as `TextureId` and `BufferId`, resolved through an explicitly
passed, App/World-owned `GpuResources`. That owner manages native resources,
dependencies, pending GPU work and cleanup. Keep `Ptr` inside the implementation
of the C boundary during migration, then enforce that boundary in the compiler.

Preserve deep-copy value semantics and the prohibition on storing `view`/`mod`
references in structs. Do not introduce general pointer arithmetic, stored
borrows, implicit shared ownership or a general `unsafe` keyword for this work.

The necessary language work is a general contract for **external resource
owners that cannot be copied**, including structural propagation and deterministic
cleanup. Typed IDs alone do not solve this. Use the existing `own`, `view` and
`mod` vocabulary; choose any required declaration spelling separately, before
implementation. This document authorizes no new syntax.

This proposal revises the GPU direction in [native handle ownership](native-handle-ownership.md)
and [WebGPU resource management](webgpu-resource-management-in-rae.md): application
IDs do not individually own or auto-release native resources. Their owner does.
Those documents remain historical proposals, not evidence of shipped behavior.

## Current implementation and gaps

- `Ptr` exists today as an opaque C-interoperability pointer, lowered to `void*`.
  Generated WebGPU bindings and handwritten platform wrappers use it. Copying
  its address cannot deep-copy the object it points to or establish ownership.
- `lib/water/WaterSystem.rae` drives resource creation and cleanup, but native
  handles reside in `g_water_ptrs[160]` in
  `compiler/runtime/runtime_gpu3d_gbuffer.c`. Separate water instances therefore
  share storage. Moving that array into `List(Ptr)` would remove the global but
  would still leave pointer copying and ownership unresolved.
- `lib/webgpu/Readback.rae` provides working nonblocking reads. Its current raw
  request handle must be released exactly once by convention; C request state
  keeps the callback alive after cancellation. Preserve that mechanism while
  replacing the public raw handle.
- The maintained Compiled target needs a reliable front-end diagnostic for
  stored `view`/`mod` fields. The earlier audit found the explicit field rejection
  in the deprecated VM compiler; the Compiled regression instead reached invalid
  C. Repair the maintained path, without doing VM work.
- Existing ownership documents mix historical syntax and implementation plans.
  Do not assume full external-owner copy/drop enforcement already exists.

## Separate data, identity and ownership

| Category | Example | Meaning of an ordinary copy | Cleanup |
| --- | --- | --- | --- |
| CPU data | `TextureDesc`, `List(Float)`, `FftSurfaceCache` | Independent contents, including owned heap data | Ordinary value cleanup |
| Resource identity | `TextureId`, `BufferId`, `SamplerId` | Same identity value naming a separately owned resource | No native release |
| Resource range | `GpuBufferRange` | Same buffer identity and numeric range | No native release |
| External owner | `GpuResources` | Rejected: there is no general deep copy of a live GPU context | Exactly one owner releases its resources |
| Temporary borrow | `view GpuResources`, `mod GpuResources` | Existing borrow rules; not a stored field | Does not release the owner |

A copied `TextureId` behaves like a copied `EntityId`: it names the same external
object. This does not make a texture an ordinary deeply copied value. Document
that distinction in names and API contracts. Updating pixels through one ID is
observable through other copies of that ID.

If independent pixels are needed, use a separately named GPU copy operation that
creates a new texture and records a supported copy. It must report unsupported
formats/usages or allocation failure and specify when the result becomes usable.
It is not an implicit implementation of `=`.

## Typed IDs and checked lookup

Start with distinct types: `TextureId`, `TextureViewId`, `BufferId`, `SamplerId`,
`PipelineId`, `BindGroupId`, `ReadbackId` and `SubmissionId`. Split pipeline types
further only where real API use benefits. Keep names in the GPU package.

Conceptual representation, not a promised binary layout:

```text
TextureId = { ownerIdentity, slot, generation }
GpuBufferRange = { buffer: BufferId, offsetBytes: Int, sizeBytes: Int }
```

Use ordinary typed wrappers initially, with runtime validation. Rae's default
cross-file visibility means a wrapper does not by itself make its fields secret
or its values unforgeable. This is memory-safety validation, not an access-control
system: code holding `mod GpuResources` is authorized to manipulate its resources.

Before any native access, check:

1. Owner identity matches this live context, and resource kind matches the API.
2. Slot is in bounds, generation matches, and the entry is live.
3. Required usage, format, mapping state and device compatibility hold.
4. Any byte range is nonnegative, aligned and in bounds, using overflow-safe checks.

Keep these safety checks in release builds. Invalid input returns an explicit
failure; it must never silently select a different resource or dereference an
unvalidated address. Diagnostics should include the operation and resource label.

Slot reuse increments its generation. Retire a slot before generation exhaustion
can make an old ID valid again. Context identity must also resist reuse: prefer
an App-owned identity allocator threaded into context creation, with checked
monotonic identities for the App lifetime. APIs operate within that App domain;
the context-creation ABI must define cross-App isolation before supporting IDs
passed between Apps. Do not derive identity solely from a reusable native address.
IDs are session-local and are not serialized asset references.

## External owner contract — approval required

Recommend one native context owner inside `GpuResources`, itself a resource on
the App or World. Metadata, caches and scheduling policy live in Rae. Native
pointer storage is per context, never a process-global renderer slot table.

The compiler must enforce these rules structurally:

- Ordinary assignment, field initialization and `copy` parameters reject an
  external owner without a valid deep-copy operation. Never silently move it.
- A struct containing such an owner, including the App, also cannot be copied.
  Putting it in `opt`, a generic container or a box must not bypass this rule.
- Construction and explicit ownership transfer use Rae's existing ownership
  model; transferred sources cannot be used or dropped again.
- `view` and `mod` temporarily borrow the owner. Neither may escape into fields.
- Scope-exit and failure cleanup run the native owner's release path exactly
  once. Unsupported container/boxing paths must be diagnosed until implemented.

Use a compiler-recognized external-owner copy/drop contract, not field-name magic
or a renderer-specific exception. It should eventually serve windows, files and
other nonduplicable external resources too. Whether it is declared through the
binding generator or a bare declaration modifier is a separate design choice.
Do not add a trait system or annotation syntax for this purpose.

An explicit `shutdownGpu` can report shutdown errors and leave the owner inert;
automatic drop remains the cleanup backstop. A documented manual shutdown alone
is not sufficient to claim that the final design is safe.

## API shape and water ownership

The following signatures illustrate the proposed library surface; they are not
currently available functions or a compilable example:

```rae
func createTexture(resources: mod GpuResources, descriptor: view TextureDesc) ret opt TextureId
func releaseTexture(resources: mod GpuResources, texture: view TextureId) ret Bool
func pollGpu(resources: mod GpuResources)
func waterDraw(resources: mod GpuResources, waterSystem: mod WaterSystem)
```

Use `opt` for creation failure and an explicit status type for operations that
need pending/success/failure distinctions. Do not require a new `Result` feature
to begin. A stale ID fails; releasing an already released nonempty ID reports
failure without touching native memory. Missing optional resources are `none`.

`GpuResources` owns all textures and buffers. A water instance holds typed IDs
and CPU data describing which resources it uses. Water creation/rebuild/shutdown
passes the manager explicitly and operates on an instance resource group there.

Crucially, copying a struct of IDs does not create a second independently owned
water simulation. A copied controller refers to the same group. Releasing the
group invalidates all its IDs; another release fails safely. Do not attach an
automatic resource-group destructor to a freely copyable controller. If a future
water controller must exclusively own and auto-release a group, it must carry a
noncopyable owner token and inherit the external-owner rules.

For the first migration, prefer manager-owned groups and explicit water teardown.
The manager remains the final cleanup owner. Create two water instances by calling
the constructor twice; do not copy one to obtain independent GPU state. CPU-only
water settings and FFT snapshots continue to have deep-copy value semantics.

## GPU completion, dependencies and readback

CPU borrow lifetimes and GPU execution lifetimes are separate. Returning from
`waterDraw` ends its CPU borrow, not the GPU's use of the recorded resources.

Use this lifecycle:

```text
live -> retired (ID immediately invalid for new work) -> physically released
```

Recording must retain resource dependencies before submission, including resources
reached indirectly through bind groups and texture views. On submission, associate
those uses with a `SubmissionId`; abandoned recordings release their holds.
Retirement must preserve resources for already recorded and submitted work. Check
dependent objects as well, so an old bind group cannot introduce new uses of a
retired texture. Release storage only after all such uses and mapping callbacks
are finished. WebGPU reference retention can implement part of this mechanism;
do not confuse native handle release with explicit resource destruction.

`pollGpu` processes events and completion without waiting for GPU work. Readback
and timing consumers use the latest completed result. Reuse bounded staging slots
only after their prior operation finishes. If every slot is busy, skip/defer new
readback instead of stalling the render loop or growing without limit.

A `ReadbackId` identifies a manager-owned request with pending, success, failure
or cancellation state. Starting a read validates range and exclusive mapping use.
Copying completed bytes borrows a destination only for that call. Never retain a
pointer into a growable Rae `List` while pending. Cancellation invalidates access
immediately but preserves callback-owned state until any late callback finishes.

On teardown, stop accepting work, cancel requests, discard unsubmitted recordings
and drain or safely transfer outstanding completion state before freeing the
native context. Explicit shutdown may wait; normal polling may not. Device loss
must resolve requests as failures and release safely under the native callback
contract. A timeout is not permission to free state still reachable by callbacks.

Completion and application generations solve different problems. Preserve water's
sea-state/resource generation and each cascade's actual sample time, even when
native IDs are valid. Late results from an old state must never populate a new
cache. Preserve modulo update cadence, the real elapsed wave clock, and the agreed
`sampleFftSurface` behavior: `none` until all active cascades have current-generation
data, or when any cascade is stale; no extrapolation and no Gerstner fallback.

## Ptr and the platform boundary

Retain `Ptr` temporarily for generated C bindings and audited interop modules.
Classify each use as native object, transient input/output address, or asynchronous
callback/request state. Replace application-facing uses with IDs or bounded
copy/upload operations. Repair necessary C lowering bugs without treating that
as approval to spread `List(Ptr)` into ordinary application structures.

After consumers migrate, reject `Ptr` in normal application declarations and
prevent trusted APIs from leaking it through return values, fields or generic
instantiations. The trust boundary must be compiler/build controlled; a folder
named `webgpu` must not grant pointer privileges to arbitrary code. Its exact
configuration is an implementation prerequisite, not an existing guarantee.

Safe wrappers validate IDs and lengths and establish resource lifetime before
calling generated bindings. Small generic C glue may own stable callback state,
hold native objects and expose completion events. Renderer policy, FFT logic,
cache choices and rebuild decisions remain Rae/WGSL. Follow the existing
[C-surface gate](webgpu-c-surface-audit.md); do not replace binding calls with a
new family of renderer-specific C helpers.

No general `unsafe` keyword is recommended now. A later trusted FFI facility
would need precise obligations and diagnostics; the keyword alone supplies no
bounds checking, ownership or callback safety.

## GPU API principles

Use explicit completion, application-controlled allocation policy and compact
draw/dispatch arguments. Aliasing copies of owning heap values and destruction
before pending GPU uses finish are unsuitable for Rae's ownership contract.

Snapshot small CPU argument blocks when recording commands while keeping their
referenced GPU resources alive through completion. Express that separation with
typed resource IDs and checked Rae/WGSL packing. A
Rae struct containing `String` or `List` is not directly uploadable shader data.
Centralize layout, alignment and packing tests; do not silently reinterpret Rae
memory as WGSL layout.

Keep the current WebGPU backend. Raw GPU addresses and writable native descriptor
heaps are not our public API contract. Do not promise native bindless behavior
or its performance through a wrapper.
Consider another backend only as a separate measured project.

## Implementation sequence and acceptance gates

1. **Approve the owner contract and define the trusted FFI boundary.** Settle
   declaration/registration details without changing the meaning of ordinary
   copies. Repair stored-borrow diagnostics in the Compiled front end.
2. **Prove external-owner semantics with a fake native resource.** Test exact-once
   drop, transfer, failure cleanup, copy rejection and structural propagation
   through App fields, optionals and supported containers before GPU migration.
3. **Implement an instance-owned GPU resource manager.** Start with buffers,
   textures and readback, validate typed IDs and dependencies, then add the
   pipeline/bind-group types water needs. Establish nonblocking completion.
4. **Migrate water as the first full consumer.** Replace numeric C slots with
   named typed fields and cascade lists. Run two independent water instances,
   shut down/rebuild one and prove the other is unaffected. Remove the water
   global table once no callers remain.
5. **Migrate shared renderer users incrementally.** Inventory compute, timing,
   grass, sprites, 2D/UI, image/font and 3D consumers. Compatibility adapters must
   delegate to the same explicit owner; they must not create another singleton
   or fake noncopyability with shallow pointers. Remove adapters after migration.
6. **Enforce the application Ptr restriction.** Reject pointer escapes and remove
   obsolete global handle accessors after maintained examples use the new API.

No promise that every example works unchanged: signatures and App construction
will change. Preserve observable behavior, keep each migration buildable, and
run all maintained Compiled example gates before declaring compatibility. Legacy
APIs require an explicit migration/deprecation choice; the deprecated VM does
not constrain this work.

Validation must cover wrong kind/owner, forged/out-of-range IDs, stale generations,
generation exhaustion, range overflow, early retirement, abandoned recordings,
bind-group dependencies, device loss, cancellation and callbacks after teardown
begins. Include repeated allocation/reuse and at least two independent requests
and contexts. Use deterministic fake-backend tests plus hardware validation.

For water, retain all 128/256/512 and 1..3 cascade combinations, runtime tier
changes, N=2/N=3 scheduling, known-wave/cache checks and buoyancy. Keep 114/118/119
visual gates green. Measure CPU overhead and GPU timings before/after on the same
hardware; do not add a blocking timing collection path to ordinary frames.

Run compiler suites only through `compiler/tools/watch-tests.sh`, one run at a
time, with an explicit timeout. Hardware checks also need explicit timeouts.
This document itself changes no runtime behavior and requires documentation
validation rather than a compiler suite.

## Decisions requested before implementation

- Adopt typed, non-owning IDs plus one explicit GPU resource owner as the default.
- Approve copy rejection and deterministic drop for external owners, propagated
  structurally; choose the declaration/registration mechanism separately.
- Keep raw pointers confined to a controlled FFI boundary; no general `unsafe`
  keyword or stored references in this project.
- Use manager-owned water resource groups first, with explicit instance teardown
  and manager cleanup as the backstop.

Implementation tasks should be queued against the approved revision, not treated
as already authorized by this design-writing request.
