# Native resource ownership

Status: **revision 3 direction accepted; exact language mechanisms pending review.**

[Ptr and GPU resources](ptr-and-gpu-resource-design.md) is the active design.
Its revision 2 module/owner registration scheme and special runtime parameter
were rejected by the maintainer. Do not implement them. Earlier versions of this
file remain in Git history rather than competing with the current proposal.

## General-purpose owner support

Any library author should be able to declare an opaque noncopyable native owner,
its cleanup operation and explicitly unsafe implementation operations in source.
The standard library uses the same rules. No privileged module inventory,
compiler-maintained per-type registration or GPU-specific language exception is
part of the revised direction.

The declaration syntax, representation hiding and cleanup mechanism remain open.
Queue #878 must show a complete small native-buffer/file example, including
failure, borrowing, transfer, rejected copies, scope-exit cleanup and optional
explicit close. These examples must be reviewed before #867/#868 implement the
language rules. Source sketches are not approved syntax.

Ordinary copies remain deep copies or errors. Owning external values must not
silently alias or move. Noncopyability propagates through enclosing structs and
generic instantiations; unsupported container/boxing operations must diagnose
rather than perform an owning shallow copy. Preserve existing view/mod lifetime
rules and the prohibition on stored borrows. Exact destruction ordering and
container support require review, not inheritance from rejected revision 2.

## GPU owners and identities

`GpuResources` owns native GPU resources; `TextureId` and `BufferId` identify them.
IDs are ordinary copyable values with no native release on drop. A copied ID
names the same resource, like a copied entity ID. Independent GPU data requires
an explicit allocation/copy operation.

The manager validates identity, kind, live slot and generation before native
access. Resource retirement invalidates new uses while retaining recorded,
submitted and callback dependencies until safe release. Native retain/release
inside that machinery does not change ordinary Rae copy semantics.

Explicit manager shutdown leaves it inert; automatic owner cleanup remains the
backstop. A copied controller containing IDs is not a second cleanup owner.
Context identity across independently created Apps remains an implementation
question for #878, without a special main signature or hidden manager singleton.

The same general owner mechanism may later serve files, windows and audio, but
this design does not authorize those unrelated migrations. For current examples,
review steps and detailed verification, use the active design document.
