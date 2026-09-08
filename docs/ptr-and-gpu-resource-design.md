# Ptr and GPU resources in Rae

Status: **revision 2, concrete contract awaiting maintainer approval, 2026-09-09.**
No compiler or runtime changes are implemented by this document. The approval
record at the end distinguishes existing language intent from new proposals.

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
`mod` vocabulary with compiler-loaded binding registration, not a new keyword.
The detailed proposal is in [the implementation contract](#implementation-contract).
The optional runtime parameter on `main` proposed there is a new entry-point
contract requiring approval, even though its spelling uses existing syntax.

This proposal revises the GPU direction in [native handle ownership](native-handle-ownership.md)
and [WebGPU resource management](webgpu-resource-management-in-rae.md): application
IDs do not individually own or auto-release native resources. Their owner does.
Their headers identify the conflicting historical sections explicitly; this
revision is the proposed replacement contract, not evidence of shipped behavior.

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
can make an old ID valid again. One explicitly borrowed runtime root supplies
non-reused context serials across all Apps in that execution, as specified below.
Do not derive identity from native addresses or random numbers. IDs are
execution-local and are not serialized asset references.

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

Use the compiler-loaded registration contract below, not field-name magic or a
renderer-specific exception. It can later serve windows, files and other
nonduplicable external resources. No trait system, general custom destructor
language feature or annotation syntax is proposed.

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
named `webgpu` must not grant pointer privileges to arbitrary code. The exact
configuration proposed below is new compiler work, not an existing guarantee.

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

## Implementation contract

### Binding registration, not a new declaration keyword

Recommend a versioned `compiler/bindings/NativeOwners.json`, compiled into the
compiler distribution with its trusted module inventory. This is a proposed
file/schema, not an existing facility. The binding generator can maintain it;
ordinary `.raepack` files cannot add privileged entries.

Each external-owner record contains these required keys:

| Key | Contract |
| --- | --- |
| `module` | Canonical slash-separated module identity, e.g. `gpu/native/NativeOwners` |
| `name` | PascalCase nominal type, e.g. `NativeGpuContext` |
| `cType` | Exact opaque native representation, e.g. `RaeNativeGpuContext*` |
| `copyPolicy` | Exactly `forbidden` in this version |
| `dropSymbol` | Native function with ABI `void dropSymbol(cType value)` |
| `emptyValue` | Exactly `null` for the initial pointer-backed ABI |
| `trustedModules` | Explicit module identities allowed to construct/unwrap this owner |

The compiler introduces that nominal type through the registered module; there
is no Rae `type NativeGpuContext { pointer: Ptr }` declaration. Duplicate source
definitions or registrations are errors. Applications may name, borrow and move
the type, or store it as an owned field, but cannot construct it with braces,
access its representation, convert integers to it or reflect/serialize its bytes.
Generated debug output names its type and state, never its address. This opacity
is part of the proposed foreign-type contract, not a claim that ordinary structs
have private fields.

The registry also lists native function contracts: exact C symbol, parameter
modes, return type, and whether untrusted Rae may call the function. Native
constructors are trusted-only and produce `opt NativeGpuContext`; success yields
one owner and failure yields `none`. Their wrapper must release any partially
created native state before reporting failure. Low-level unwrapping and release
operations are never application-callable. A safe Rae factory returns the enclosing
`GpuResources`, which holds the opaque context plus Rae metadata.

The compiler validates duplicate names, missing symbols/contracts, incompatible
drop signatures and forbidden copy parameters before emitting C; C compilation
and linking verify the registered ABI. Only trusted binding implementation code
may adapt the nullable native result into the typed optional. Native callbacks
cannot synthesize extra Rae owner values for an already owned allocation.

### Copy, transfer, return and partial construction

An external owner is noncopyable; an aggregate is noncopyable if any of its owned
fields or concrete generic elements is noncopyable. This property is independent
of whether the object owns a Rae heap allocation. Do not implement it by merely
extending `type_owns_heap_storage` or by letting a recursion-depth limit classify
an unknown type as copyable. Use resolved nominal types and cycle-aware analysis.

The following rules are proposed requirements for external owners and aggregates
containing them. Illustrations use existing Rae expression/parameter spellings;
they are not claims that all checks work today.

| Operation | Required behavior |
| --- | --- |
| `let resources: GpuResources = createGpuResources(...)` | Take a produced owned result; no copy of an existing owner |
| `let other: GpuResources = resources` | Reject, even if `resources` is never used again |
| `let other: GpuResources = own resources` | Transfer a whole owned local; source becomes unavailable |
| `field: resources` | Reject copying an existing owner into a field |
| `field: own resources` | Transfer the local into the new aggregate |
| Passing to `view`/`mod` | Borrow for the call; no transfer or cleanup by the borrower |
| Passing to `own` | Transfer a whole owned local or produced result; the declared parameter makes consumption explicit |
| Passing to `copy` | Reject, including a fresh temporary; the signature promises an unsupported operation |
| `ret resources` from an owned local/`own` parameter | Transfer out under Rae's existing owned-return convention; do not drop it locally |
| Returning an owner through `view`/`mod` | Existing reference-return lifetime rules apply; never turn a borrowed owner into an owned result |
| Assigning a produced/transferred owner over a live mutable owner | Evaluate RHS first, then drop old destination once, then install new owner |

Reject transfers from borrowed places, self-moves and taking a field/element out
of a live aggregate in the first version. Partial moves need separate support;
do not leave a hole that generated drop later traverses. A helper requiring an
owned argument cannot consume a field through its parent's `mod` borrow.

Track initialization and movement through branches and loops. A subsequent use
requires the owner to be initialized and not moved on every incoming path.
Conditional cleanup uses initialization flags; returning on one branch and
dropping on another must each release exactly once. A loop cannot consume the
same owner repeatedly without definite reinitialization.

Construction initializes fields in written initializer order; omitted defaults
follow in declaration order. If construction exits early, drop only initialized
owned fields, in reverse initialization order. Normal aggregate drop visits
fields in reverse declaration order; locals drop in reverse initialization order.
These evaluation/drop ordering requirements are proposed for this contract and
must be checked against ordinary aggregate behavior before implementation; they
are not permission to silently reorder existing observable behavior. Put the
native context field first in `GpuResources` so it drops after the Rae metadata.

`opt Owner` supports construction, borrowing, whole-optional transfer and drop.
A produced optional can transfer its payload into an owned `if let` branch;
a stored optional can only be borrowed unless it is transferred as a whole to
an owning helper first. Dropping `none` does nothing. Returning `none` from a
factory drops every initialized temporary owner still in that factory.

Initial container support is deliberately bounded: ordinary owner fields and
optionals are required; `List(Owner)`, `Array(Owner)`, maps, ECS component storage
and `Any` boxing are rejected until their element operations have proven transfer
and drop support. This also applies when an element indirectly contains an owner.
`List(TextureId)` and `List(BufferId)` remain ordinary copyable containers.
App/World resources are direct fields; moving the whole App is permitted, copying
it is rejected. This restriction does not forbid existing containers of CPU data.

Drop is nonthrowing and receives ownership of the native value exactly once.
Generated code clears a transferred or dropped storage slot to the registered
empty representation; empty is compiler-internal, not a public default constructor.
Native drop accepts empty defensively. Explicit `shutdownGpu(resources: mod
GpuResources)` transitions the native context to an inert closed state, releasing
GPU resources; its small owner object remains until normal drop. Closing twice
is harmless; resource operations on a closed owner fail. There is no resurrection.

Normal lexical exits and explicit failure returns get cleanup. Process kill,
abort and OS termination do not promise language-level destructors. Recoverable
GPU errors must use failure results and leave owners valid or closed.

### One explicit runtime identity authority across Apps

Recommend an execution-owned `RuntimeResources` root with a checked `UInt64`
context counter. The compiled launcher owns it for the entire execution. It is
noncopyable, has no public constructor and is never reachable through a global
getter. Context creation temporarily borrows it to reserve a fresh serial; the
resulting context does not store a Rae reference to it.

To expose that authority without hidden mutable state, propose this optional
entry-point signature, alongside the existing parameterless `main`:

```rae
func main(runtime: mod RuntimeResources) {
  # Construct Apps using factories that receive runtime explicitly.
}
```

The launcher passes its root as an ordinary borrow. This is a **new entry-point
semantic requiring maintainer approval**, not a new keyword or a silently supplied
parameter on ordinary calls. Parameterless programs keep working; migrated GPU
programs use the explicit parameter. `main` with runtime injection is a launcher
entry point, not an ordinary callable factory for additional runtime roots.

All Apps in one execution receive the same explicit root through their constructors.
Two Apps creating their first GPU context receive different nonzero serials;
closing/recreating a context never reuses a serial. There is no App-local counter
that restarts at one, no random-UUID collision assumption and no pointer-derived
identity. The counter lives in the launcher-owned object, not a C static variable.
Initially creation and polling stay on the owning runtime thread.

Use `UInt64` owner identity, slot and generation fields in the initial typed IDs;
avoid packing until measurement justifies it. Zero owner identity/generation is
invalid. Slots may start at zero. Allocate serials 1 through the maximum value
once, then return creation failure permanently. A slot generation starts at one;
after its last representable generation is retired, never reuse that slot.
Retirement changes live state immediately; reuse advances generation before
publishing a replacement. Never wrap any identity or submission sequence.

IDs are valid only within this execution. Serialization is not an interchange
mechanism: export an asset/resource description and recreate it to get a local
ID. A native embedding must use one host-owned runtime root for all Rae Apps
that exchange IDs. Multiple isolated runtime roots cannot exchange IDs through
the safe API. Cross-process/shared-library transport is outside this contract;
trusted C code cannot bypass this restriction and still claim safe-ID guarantees.

The registered context constructor requires a borrow of the runtime root; it
cannot allocate an independent identity namespace. Moving an App/context preserves
its identity. A stale or foreign ID fails before native lookup, even if its slot
and generation happen to match a resource in another App.

### Exact trusted-module configuration

Compile a versioned inventory into the compiler distribution alongside the owner
registry. Each entry identifies a canonical module, its path under the configured
stdlib root and its content digest. Trust requires all three to match the bytes
actually parsed. Resolve symlinks and reject paths escaping the root; do not
trust a module selected earlier in the application import search path merely
because it has the same name. Include the inventory digest in build-cache keys.

Changes to trusted modules regenerate the inventory as part of the compiler build
and are reviewable source changes. Test-only fake owners get a dedicated test
compiler inventory; a normal `.raepack`, source annotation or imported package
cannot extend it. A future third-party FFI installation workflow needs a separate
trust decision; no general user-extensible mechanism is introduced here.

Trust is not transitive through imports. Audit every call from untrusted code,
including aliases and callable values. Raw `extern` declarations and direct
calls to trusted-only operations are rejected outside the inventory after the
migration. Explicitly registered safe native functions may remain callable with
validated value/owner signatures. This is a foreign-call safety boundary, not a
general private/public module system.

Trusted modules can contain raw `Ptr` and C descriptor layouts. Their safe
exports cannot expose raw pointers through fields, returns, generic substitution,
reflection or callback captures. A registered opaque owner is the only approved
foreign representation that may cross while hiding its native pointer. Generated
safe API documentation marks owner types noncopyable even without source keywords.

Native asynchronous callbacks own stable native request state and retained native
objects. They may publish completion into that state, but may not retain borrowed
Rae locals, list elements, or an address of movable owner storage. Explicit shutdown
may wait and pump events; normal polling must not. If a backend needs completion
after shutdown returns, ownership must be transferred to its documented callback
state, including every native dependency needed for completion. It cannot leave
callbacks pointing into the destroyed Rae manager. A backend with no such safe
contract must drain before dropping its owner; timeout cannot force a dangling free.

### Acceptance examples for the implementation review

| Case | Required result |
| --- | --- |
| Copy `TextureId`; drop both copies | No GPU release |
| Copy an App carrying `GpuResources` | Compile error before C generation |
| Return a newly built App; move it into an owning call | One eventual native context drop |
| Fail after creating two of three fields | Two drops, reverse initialization order |
| Move only on one branch then read after the join | Compile error |
| Store an owner indirectly in `List` or `Any` | Explicit unsupported-owner diagnostic in version one |
| Create contexts in two Apps, both using slot zero | Different owner serials; cross-App ID use fails |
| Exhaust serial or generation in a reduced-width fixture | Failure/slot retirement, never wraparound |
| Shadow a trusted module path or change its source bytes | No trust privilege |
| Shutdown with a pending mapping callback | No pointer to destroyed Rae storage; exactly-once native cleanup |

For #867, fake-owner tests can use a test-only registration fixture implementing
this schema. #868 supplies production trust configuration and the audit mode;
#877 makes enforcement universal after consumer migrations. Neither task may
infer approval from a draft registration example.

## Approval record and remaining gate

Existing maintainer direction: no ordinary pointers or stored borrows, deep-copy
value semantics, interest in typed IDs, and authorization to write this design
and queue its implementation. These statements do **not** approve new foreign-type
opacity, noncopyable-type enforcement, binding registration or runtime injection.

Revision 2 selects concrete recommendations for review:

1. Registered opaque external owners, structural copy rejection, exact-once drop,
   the bounded container rules and the transfer/return/failure rules above.
2. Compiler-distributed trusted module/function registration; no new keyword,
   general `unsafe`, app-defined trust or mutable global handle table.
3. Optional `main(runtime: mod RuntimeResources)` injection of one launcher-owned
   identity authority; context/generation counters never wrap or restart across Apps.
4. Copyable non-owning IDs and manager-owned water groups, with explicit water
   teardown and the manager's deterministic drop as the backstop.

**Maintainer approval: pending.** In particular, item 3 expands the original
proposal to resolve cross-App identity without hidden globals. Do not implement
that entry-point change, the drop ordering contract or the registered-owner
semantics until this revision is approved. If a different bootstrap is preferred,
it must still provide one explicit identity authority to Apps that exchange IDs.

Queue #865 records this review gate as a question until approval is received.
The independent existing-rule compiler repairs #866/#864 do not require approval
of this proposal; dependent semantic work does. No runtime suite is needed for
this documentation-only revision; link, consistency and diff checks are required.
