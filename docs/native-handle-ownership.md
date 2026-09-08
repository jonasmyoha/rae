# Native resource ownership

Status: **historical proposal reconciled with the revision 2 GPU contract;
maintainer approval of new semantics is pending.**

The earlier design under runtime migration task #289 described individually
owning native handles. For the GPU work, its replacement is
[Ptr and GPU resources](ptr-and-gpu-resource-design.md), especially the
[implementation contract](ptr-and-gpu-resource-design.md#implementation-contract).
That document is the single proposed authority for copy/drop rules, binding
registration, runtime identity and trust. The original draft remains in Git
history; it must not be implemented as a competing GPU ownership model.

## What changed

| Earlier proposal | Revision 2 recommendation |
| --- | --- |
| Each `GpuTexture`/`GpuBuffer` value owns and auto-drops a native object | `TextureId`/`BufferId` are copyable identities; `GpuResources` owns the objects |
| A generic integer `NativeHandle` field identifies ownership | Registered nominal external-owner types carry compiler-known noncopyability and drop behavior; no field-name convention |
| Runtime tables without an explicit instance requirement | Native storage belongs to a specific context, owned by the App/World resource manager |
| Table kind, slot and generation validate a handle | Also validate execution-wide context identity and live/retired state before native use |
| Borrowing an object until command recording ends is sufficient | Retain recorded and submitted dependencies through GPU completion and mapping callbacks |
| Generic containers/boxing should immediately support owning handles | Version one requires direct fields and `opt`; unsupported owner containers/boxing are rejected |
| Destructor metadata mechanism left open | Compiler-loaded binding registry, with the exact schema and trusted-module rules in the replacement contract |

## Owner and identity are different types

An ID owns no GPU allocation. Copying or dropping IDs does not retain or release
textures. A manager may retire a texture while IDs still exist; those IDs then
fail validation. IDs are not unforgeable capabilities or serialization keys.

The registered native context inside `GpuResources` is an external owner.
Ordinary copies of that owner or an enclosing App are rejected. Existing `own`
transfer and `view`/`mod` borrowing remain the vocabulary. Native reference
counting may retain in-flight dependencies internally; it does not make ordinary
Rae owner assignment a shared-reference operation.

Explicit shutdown leaves an owner inert; normal drop is the cleanup backstop.
Native objects and callback state must remain alive for outstanding work.
A copied water controller has copied IDs, not a second independent cleanup owner.

## Other platform resources

The same registered external-owner mechanism may later serve windows, files,
audio devices and process handles, but their migration is not approved or
implemented by this GPU proposal. A separately owning native object would use
the noncopyable owner contract. A manager-issued identity would use the ID
contract. Do not silently conflate them or grant every native wrapper custom
copy behavior.

No `SharedHandle` type, new destructor keyword, trait system, raw-pointer app API
or stored borrow is introduced by this revision. Constructors, field opacity,
explicit transfers, failure cleanup and trusted native calls must follow the
replacement contract after approval.

## Implementation and verification

Use queue #865 for approval, #867 for fake native owner copy/drop tests and #868
for the controlled FFI boundary. The GPU manager, completion and consumer tasks
follow those prerequisites. Existing generated WebGPU bindings and the generic
readback bridge remain migration inputs, not proof that compiler-enforced owner
semantics are already shipped.

Verification includes exact-once cleanup, rejected aggregate copies, safe failure
returns, stale/wrong-context IDs, cancellation after shutdown begins, and two
independent Apps/contexts. The replacement contract defines detailed acceptance
cases. Do not restore the old assumption that `List(Handle)` is safe merely
because its elements look like small integers or pointers.
