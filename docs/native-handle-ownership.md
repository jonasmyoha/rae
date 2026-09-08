# Native resource ownership

Status: **revision 3 direction accepted; concrete revision 4 awaits approval.**

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

Revision 4 now supplies a complete C demonstration library, its Rae wrapper and
an ordinary `Main.rae` caller. It proposes `type NativeBuffer: opaque noncopyable
drop`, a defining-module `func drop(this: mod NativeBuffer)` hook, `unsafe` blocks
and a trailing `unsafe` function property. These are concrete proposed semantics,
not already available language features. Review the
[complete example](ptr-and-gpu-resource-design.md#complete-native-buffer-example)
before implementing #867/#868.

Ordinary copies remain deep copies or errors. Noncopyability propagates through
owned fields and optionals; version one explicitly rejects owner containers and
boxing until their transfer/drop operations are implemented safely. CPU data and
lists of IDs remain copyable. Opacity prevents representation access outside the
defining module, including in unsafe blocks; every module can define such a type.
No stored Rae references are permitted.

The opt-in hook runs before normal field cleanup, and direct user calls to it are
rejected. An idempotent `close` is a separate public operation. Existing ordinary
cleanup order is preserved; the proposed hook ordering does not reinstate the
rejected revision 2 ordering contract. Approval of this exact behavior is pending.

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
For context identity, revision 4 recommends fresh 256-bit OS-random nonces with
checked generations. Its cross-context uniqueness guarantee is probabilistic,
not absolute, and requires explicit approval. The design compares alternatives
and includes two independently created Apps with the same slot/generation,
without a special main signature or hidden manager singleton.

The same general owner mechanism may later serve files, windows and audio, but
this design does not authorize those unrelated migrations. For current examples,
review steps and detailed verification, use the active design document.
