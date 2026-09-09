# Native resource ownership

Status: **use the shipped create/drop/copy contract; the remaining pointer
boundary is proposed in revision 6 of the GPU design.**

[Constructors, destructors and copies](constructors-and-destructors.md) defines
Rae's implemented lifecycle model. A native owner uses ordinary `create` and
`drop` functions and normally has no `copy`. A custom copy must produce an
independent value. Containers follow element ownership; there is no additional
owner property, registration or blanket container prohibition.

The rewritten [GPU and C-interop proposal](ptr-and-gpu-resource-design.md) builds
on that model. It recommends marking raw pointer operations and foreign-call
obligations explicitly in source, equally available to any module. Its current
proposal needs neither privileged module lists nor an opaque-type keyword:
reading or installing the native pointer itself would require unsafe code.
These additional operation rules still need approval and implementation.

This does not create general field privacy. Safe wrappers must validate against
authoritative native bounds/state rather than trusting publicly mutable metadata.
Review generic access, reflection, serialization and raw callback formation as
well as direct field access. Lifecycle hooks alone cannot prove native contracts.

GPU IDs are copyable identities with no native release on drop. An App-owned
manager controls their resources, groups and asynchronous dependencies. Native
reference retention for outstanding work is not an independent value copy.
The manager-identity representation remains a library design choice; no fixed
random token layout or special main parameter is approved.

The first native-owner pilot now wraps the nonblocking readback request bridge.
It verifies exact-once cleanup, explicit drop, transfer, owner fields and Lists,
cancellation and late callbacks. The pilot is not itself a completed safe-pointer
boundary: its raw destination and native request operations remain internal.

The older ownership-property and registered-native-owner proposals are superseded.
Use the linked documents as the current contracts; historical text remains in Git.
