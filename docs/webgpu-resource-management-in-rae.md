# WebGPU resource management in Rae

Status: **revision 7 approved interop contract over the shipped
create/drop/copy lifecycle, with staged source-file enforcement implemented;
GPU manager identity contract proposed in
[gpu-resource-identity-design.md](gpu-resource-identity-design.md) (#892), approved 2026-09-11; the instance-owned `gpu/GpuResources` manager is
implemented (#869).**

The active [GPU and C-interop design](ptr-and-gpu-resource-design.md) has been
rewritten. Use ordinary lifecycle functions from
[constructors, destructors and copies](constructors-and-destructors.md); do not
implement the old property-based owner scheme, owner registration or runtime
injection into main.

`GpuResources` is an App/World-owned value with `create` and `drop`, normally no
`copy`. `TextureId` and `BufferId` identify resources in it. Copying an ID does not
allocate, retain or release that resource. Duplicate native data through a named
fallible operation; native retain alone is not a deep copy.

Rae owns descriptors, validation, groups, caches, dependencies and scheduling.
Use generated WebGPU bindings and generic stable callback glue for the native
boundary. The approved source-level unsafe facility marks raw pointer access and
foreign obligations in any module. Function modifiers follow `)`, and every
extern explicitly includes the distinct `unsafe` modifier before `extern`. This
does not grant special privileges to stdlib or make public cached bounds
authoritative.

Resource uses start at recording, include bind-group/view dependencies and finish
only after submission/completion or abandonment. Retired IDs reject new uses;
existing uses keep native resources alive. Normal readback/timing polling remains
nonblocking with bounded staging. Shutdown and late callbacks require an explicit
native lifetime protocol even when Rae calls the destructor automatically.

Manager IDs must reject wrong-owner and stale use according to a reviewed identity
contract. Compare representations using two independently created managers; no
fixed 256-bit nonce or global issuer is required by the current design. State any
probabilistic guarantees honestly. Keep normal main and explicit owner parameters.

The lifecycle wrapper around existing readback is complete as an internal pilot.
Next establish the reviewed raw pointer boundary and checked manager before
migrating water and other consumers.
Manager-owned water groups allow independent instances; a copied controller of
IDs does not acquire a destructor that releases another copy's resources.

Keep existing FFT tiers/cadence/cache semantics, C-surface gate, maintained example
and screenshot gates, and matched performance measurements. Suites run through
`compiler/tools/watch-tests.sh`, one at a time with explicit timeouts; hardware
checks also have timeouts. New water features and backend changes remain separate.
