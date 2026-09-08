# Ptr and GPU resources in Rae

Status: **revision 3, 2026-09-09. Direction accepted; exact language mechanisms
remain to be designed and approved with concrete examples.** No implementation
is claimed by this document.

## Decisions from review

Use typed non-owning resource IDs and an explicitly owned GPU resource manager.
Develop a general-purpose explicit low-level boundary, such as `unsafe`, available
to any library author. Keep ordinary Rae data deeply copied, and prohibit stored
`view`/`mod` references. External owners must not silently alias through copying.

The maintainer rejected revision 2's privileged module inventory, compiler-loaded
owner registration scheme and runtime injection into `main`. They are withdrawn,
not optional alternatives awaiting implementation. Keep normal `func main()`.
No module path, standard-library membership or special build registration should
be needed to author a binding. The standard library follows the same language
rules as other libraries.

The accepted direction does not settle the spelling of `unsafe`, native owner
declarations or cleanup hooks. Those require a small general-purpose example and
explicit review before compiler changes. Do not implement a keyword or choose a
new destruction order based solely on the sketches below.

## What an ordinary app looks like

All code below illustrates proposed APIs. It is not a compilable example of
currently shipped GPU ownership support. The first sketch omits GPU-context
creation failure handling to focus on the relationship between owner and ID;
the final API must define that failure path before implementation.

```rae
func main() {
  var resources: GpuResources = createGpuResources()

  if let texture: TextureId = loadTexture(
    resources: resources,
    path: "water.png"
  ) {
    drawTexture(resources: resources, texture: texture)
  }
}
```

`resources` owns the native context and its resources. `texture` identifies one
texture in that manager. Dropping the ID does not release the texture. Leaving
the manager's scope performs its cleanup, including safe handling of pending GPU
work. There is no implicit current manager and no special parameter on `main`.

In a larger program, put the manager on the App or World and pass it explicitly
through `view`/`mod`, just like other application state. No global water handle
array is needed.

## Copying an ID versus copying a resource

```rae
let first: TextureId = texture
let second: TextureId = first
```

These are independent copies of the identity value. They name the same texture,
just as two copies of an `EntityId` name the same entity. Neither owns the texture.
Changing its pixels through one ID is observable through the other.

```rae
let duplicate: GpuResources = resources
```

Reject this copy: a live context has no general independent deep copy. Do not
silently share its pointer, retain a shared owner, or move the source because it
happens not to be used again. An App containing the manager also cannot be copied.

To request independent texture contents, use a separate operation:

```rae
let duplicate: opt TextureId = duplicateTexture(
  resources: resources,
  source: texture
)
```

This creates another allocation and records a supported GPU copy. Its contract
must describe failure and when the result is usable. It is not ordinary `=`.

| Value | Copy behavior | Cleanup |
| --- | --- | --- |
| `TextureDesc`, `List(Float)`, CPU FFT snapshot | Deep-copy owned data | Normal value cleanup |
| `TextureId`, `BufferId`, `SamplerId` | Copy identity; refer to same separate resource | No native release |
| Buffer ID plus offset and length | Copy the bounded range description | No native release |
| `GpuResources` or App containing it | Reject ordinary copy | Exactly one owner cleans up |
| `view`/`mod` parameter | Temporary borrow under existing rules | Does not own or release |

## Two water instances

```rae
type WaterTextures {
  displacement: TextureId
  normal: TextureId
}
```

A water controller holds these IDs and CPU simulation data. `GpuResources` owns
its textures, buffers, pipelines and pending requests, grouped by water instance.

```rae
var lake: WaterSystem = createWaterSystem(resources: resources, body: lakeBody)
var ocean: WaterSystem = createWaterSystem(resources: resources, body: oceanBody)

waterStep(resources: resources, waterSystem: lake)
waterStep(resources: resources, waterSystem: ocean)
waterShutdown(resources: resources, waterSystem: lake)
waterStep(resources: resources, waterSystem: ocean)
```

Separate constructor calls allocate separate groups. Shutting down the lake does
not affect the ocean. Copying a controller merely copies its resource identities
and deeply copies its CPU data; it does not produce an independent GPU simulation.
Consequently, a freely copyable controller must not automatically release its
resource group when a copy leaves scope. Use explicit group teardown, with the
manager's cleanup as the backstop. An exclusively owning controller would instead
need to inherit the noncopyable-owner rules.

Releasing a group invalidates all of its IDs. A copied controller holding those
IDs then fails validation safely. Shared controller copies are not independent
simulations and should not be presented as such in the API.

## A low-level boundary any library can use

Recommend explicit low-level operations in source, with no privileged module list.
Conceptual syntax only, not an approved grammar:

```rae
unsafe {
  # Call C using its native representation.
  # The binding author establishes pointer validity, bounds and lifetime.
}
```

A safe `loadTexture` or buffer-upload function validates its arguments and owns
any required temporary storage before entering such a block. Its callers use an
ordinary typed API. A third-party binding author has exactly the same facility.

The design review must distinguish two responsibilities:

- An unsafe operation is one the compiler cannot prove valid, such as a raw native
  memory access or a foreign call with unchecked pointer/lifetime requirements.
- An unsafe function exposes obligations to its caller. A safe wrapper can contain
  unsafe implementation operations while satisfying those obligations itself.

The declaration spelling and call checking for these cases are still open.
`extern` names a foreign ABI; it does not by itself prove that a call is safe.
Decide how safe foreign functions state their contract and how callback/callable
values preserve unsafe requirements. Do not assume every C call has identical
obligations or make importing a module an implicit permission grant.

`unsafe` must not silently change ordinary copying, disable all type checks or
allow a borrowed Rae reference to be stored in a struct. Ordinary GPU code uses
IDs and bounded copy/upload operations. Raw `Ptr` remains a possible low-level
interop representation, not the recommended way to structure app state.

Exactly where raw pointer types may be declared or stored, and how their native
representation is hidden behind an owner, must be demonstrated in the source-level
owner example below. Do not substitute a module allowlist for that design work.

## Design the owner using a small example first

Before adapting the renderer, work through an owned native buffer or file using
an ordinary user-authored module. Show the complete source for:

1. Declaring its opaque native representation and that it cannot be copied.
2. Creating it, including allocation failure and partial-construction cleanup.
3. A safe operation using a temporary `view`/`mod` borrow.
4. A small explicitly unsafe implementation operation and its caller obligations.
5. Moving the owner with existing `own` syntax and returning it from a factory.
6. Rejected copying, access to hidden representation and use after transfer.
7. Deterministic cleanup and optional explicit close without double release.
8. Storing the owner in another struct, an optional and a supported container,
   or an explicit diagnostic where container support is deferred.

This must be a general language facility: library-authored owner declaration and
cleanup, visible in source, without a compiler-maintained per-type registration
file, native field-name magic or a GPU-specific compiler exception. Opacity is a
real remaining language question because ordinary Rae fields are visible across
modules today. A wrapper around a public `Ptr` does not solve it by itself.

Preserve existing explicit parameter modes and ownership-transfer conventions.
Ordinary assignments and field initialization copy or fail; they never silently
move. Audit return behavior, partial initialization, branch/loop moves, replacement
assignment and existing destruction order before proposing precise rules. Version
2's particular ordering and blanket owner-container restrictions were not approved.
Unsupported paths must fail clearly rather than perform shallow owning copies.

An explicit close operation should leave the owner inert. Automatic cleanup is
the backstop, not a second independent release. Noncopyability must propagate
through enclosing types, optionals and generic instantiations so boxing or a
container cannot accidentally evade it. CPU data and lists of IDs remain copyable.

## IDs and independent contexts

Start with distinct `TextureId`, `TextureViewId`, `BufferId`, `SamplerId`,
`PipelineId`, `BindGroupId`, `ReadbackId` and `SubmissionId` values. Conceptually,
a resource ID carries an owner identity, slot and generation; packing is not
specified yet. A range carries a `BufferId`, byte offset and byte length.

Validate kind, correct owner, bounds, generation, live state, usage and alignment
before native access. Arithmetic must be overflow-safe. Keep safety checks in
release builds. An ID is not an unforgeable security capability; holding the
mutable manager authorizes operations on its resources. Never dereference an
unchecked address merely because it arrived in an ID-shaped wrapper.

Cross-context identity is an unresolved implementation decision, not a reason to
change `main`. The design task must show two managers created by ordinary calls,
including managers in separate Apps, and prove wrong-owner rejection even when
slot and generation match. It must also handle context destruction/recreation
and generation exhaustion without making an old ID valid again. An App-local
counter starting at one is insufficient; neither a reusable address nor an
unstated random-collision assumption is a proof. If an option has a probabilistic
guarantee or extra lifetime cost, state it explicitly for review.

No special runtime parameter, hidden current-manager API or mutable global GPU
resource table is approved. Compare practical identity representations before
selecting one. IDs are execution-local, not serialized asset references.

## GPU completion and cancellation

A CPU borrow can end before the GPU finishes using recorded resources. The
manager must retain resources from recording through submission and completion,
including indirect dependencies through bind groups and views. Abandoned
recordings release their holds. Retired IDs reject new uses immediately; native
storage is released only when recorded/submitted work and callbacks permit it.
Native reference retention is an implementation mechanism, not implicit shared
ownership of ordinary Rae owner values.

Normal polling never waits for GPU completion. Readback and timing use bounded
staging slots; if all are busy, skip/defer additional sampling. Consumers use the
latest completed result. Keep explicit blocking diagnostics separately available.

A readback request owns stable native callback state. Copy completed bytes into
a borrowed destination only during the copy call; never retain a pointer into a
growable Rae list or movable owner. Cancellation invalidates access immediately,
but a late callback must still have valid state and release its ownership once.

Shutdown stops new work and cancels/drains or safely transfers outstanding native
callback state before the Rae owner disappears. It may wait; normal polling may
not. Device loss resolves requests as failures. A timeout does not permit freeing
callback-reachable memory. Any deferred teardown must retain all its native
dependencies and must not depend on a global orphan-request registry.

Keep resource generation separate from water's sea-state generation and actual
per-cascade simulation sample times. Preserve modulo cadence, mobile default N=2,
real elapsed wave time and the existing optional FFT query: none until every
active cascade has current-generation data, or if any sample is too old; no
extrapolation and no Gerstner fallback.

## Native boundary and GPU API scope

Keep WebGPU and its generated bindings. Rae owns descriptors, cache decisions,
resource groups, dependencies, scheduling and rebuild policy. Generic C glue may
hold native objects and stable callback state. Follow the existing
[C-surface gate](webgpu-c-surface-audit.md); the new language boundary does not
permit renderer policy to migrate into new renderer-specific C helpers.

Use compact typed draw/dispatch arguments and centralized checked Rae/WGSL packing.
Snapshot command parameters when recording; retain referenced GPU resources until
completion. Do not upload the raw representation of Rae strings, lists, owner
objects or resource ID validation fields as shader data. No new backend or
bindless performance promise is part of this migration.

## Current evidence and migration order

`Ptr` currently lowers to `void*` in the C backend. Water still stores native
handles in `g_water_ptrs[160]` in `compiler/runtime/runtime_gpu3d_gbuffer.c`.
The existing `webgpu/Readback` bridge already owns callback state, but its public
raw request ownership is conventional rather than enforced by an owner type.
Moving the slots into `List(Ptr)` alone would leave copy ownership unresolved.

1. Review the complete native-buffer/file example, exact unsafe/owner/cleanup
   rules and context identity options. This is queue #878, replacing the rejected
   revision 2 approval question. Do not infer syntax approval from this revision.
2. Repair existing-rule compiler bugs #866/#864 independently. Use fake-resource
   tests to implement the approved general owner and low-level boundary in
   #867/#868; neither depends on a privileged registration scheme.
3. Build checked instance-owned GPU resources, dependency tracking, nonblocking
   readback/timing and argument packing (#869-#872).
4. Migrate water first (#873), then shared 3D, 2D/UI and remaining consumers
   (#874-#876). Keep maintained examples buildable between stages.
5. Complete enforcement of the approved explicit low-level boundary (#877).
   Any module can author such code; safe API use must not require it.

Water feature additions remain postponed. The ownership migration is active.
Reassess the postponed raw Ptr lowering bug #863 only if a concrete interop
consumer requires it; it is not approval to spread pointer storage into app state.

Verification includes exact-once drop and copy rejection, nested owners and
failure returns, two independent contexts/water groups, stale IDs and exhausted
counters, callback cancellation after teardown begins, all FFT tiers and cadence,
known-wave/cache checks, buoyancy and inspected 114/118/119 gates. Measure CPU/GPU
costs on matched hardware. Compiler suites use `compiler/tools/watch-tests.sh`,
one run at a time with an explicit timeout; hardware runs also need timeouts.

## Approval boundary

The maintainer accepted the revised direction after rejecting revision 2:
normal `main`, no privileged module/owner registry, typed IDs, explicit resource
ownership, and a general source-level low-level facility worth designing.
Exact unsafe syntax, foreign-owner opacity, cleanup declaration/ordering,
container support and cross-context identity remain pending the example-led
review. No new compiler/runtime semantics were implemented in this revision.
