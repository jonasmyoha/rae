# GPU resources and C interop using Rae's lifecycle functions

Status: **new proposal, revision 5, 2026-09-09.** Rewritten around the shipped
`create`, `drop` and `copy` model. The remaining low-level rules below are
recommendations for review, not implemented or approved language changes.

## Recommendation

Build the GPU library with ordinary Rae types and lifecycle functions:

- A resource owner defines `create` and `drop`. It normally has no `copy`.
- `TextureId` and `BufferId` are ordinary copyable identities into that owner.
- The App owns the resource manager and passes it explicitly to systems.
- Native pointers are implementation data. Propose explicit `unsafe` operations
  for inspecting or manipulating them, available to every library author.

No ownership properties, type registration, special module privileges or runtime
parameter on `main` are needed. Do not introduce a separate opacity feature as a
prerequisite: first test whether restricting raw pointer access itself provides
an adequate boundary for these wrappers.

This replaces the previous GPU proposal, not the shipped lifecycle contract.
[Constructors, destructors and copies](constructors-and-destructors.md) is the
source of truth for those semantics. Historical queue wording or earlier drafts
must not be used to reintroduce removed properties or container restrictions.

## What already works

Recent compiler work provides constructors by expected type, destructors by
name/signature, custom independent copies, cleanup of temporaries and rejection
of stored `view`/`mod` fields. Explicit `value.drop()` consumes a value. Owners can
live in fields and supported containers; copying follows their element rules.

The existing [resource lifetime example](../examples/120_resource_lifetimes/Main.rae)
acquires a native mixer slot in `create`, releases it in `drop`, and implements
`copy` by acquiring a different slot. It demonstrates lifecycle ownership, not
complete protection against forging native handles or asynchronous GPU hazards.

This is enough to express the ownership half of a native wrapper. We should
exercise and fix that machinery rather than add another ownership system.

## An ordinary graphics application

These GPU APIs are proposed; the lifecycle syntax is already Rae. All examples
in this document describe intended APIs rather than runnable GPU examples.
The `GpuResources` constructor, texture loading/drawing and window polling are
the proposed library operations; this example shows the App and system that own
and use them.

```rae
type RenderSystem {
  resources: GpuResources
  texture: opt TextureId
}

func create(texturePath: view String) ret opt RenderSystem {
  if let resources: GpuResources = GpuResources.create() {
    var renderSystem: RenderSystem = {
      resources: own resources,
      texture: none
    }
    if let texture: TextureId = loadTexture(
      resources: renderSystem.resources,
      path: texturePath
    ) {
      renderSystem.texture = texture
      ret renderSystem
    }
    # Texture loading failed. The local renderSystem drops its resources.
  }
  ret none
}

func drawFrame(this: mod RenderSystem) {
  if let texture: view TextureId => this.texture {
    drawTexture(resources: this.resources, texture: texture)
  }
}

type App {
  renderSystem: RenderSystem
}

func create() ret opt App {
  if let renderSystem: RenderSystem = RenderSystem.create(texturePath: "water.png") {
    ret App { renderSystem: own renderSystem }
  }
  ret none
}

func frame(app: mod App) {
  drawFrame(this: app.renderSystem)
}

func main() {
  if let app: App = App.create() {
    loop not shouldClose(resources: app.renderSystem.resources) {
      frame(app: app)
    }
    # app leaves scope here; its owned fields are cleaned up automatically.
  } else {
    log("App creation failed")
  }
}
```

The ownership tree is explicit:

```text
main's app
  renderSystem
    resources   -> owns the GPU context and actual texture allocation
    texture     -> stores an optional ID naming that allocation
```

The App and all of this state survive across frames. `frame(app: mod App)` and
`drawFrame(this: mod RenderSystem)` temporarily borrow existing state; neither
receives ownership or constructs another manager. `own` appears at construction
boundaries to put each value into its lasting owner without copying it. Returning
the owned local `renderSystem` transfers it out under the existing return rules.

`if let app: App = App.create()` already takes ownership of the produced optional
payload. The then-branch owns `app`; it needs no second binding or `own` transfer.
An owned `let` binding can be borrowed through `mod` to update its contents;
`var` is needed if the binding itself must be reassigned. Exiting the branch,
including an early return, drops the owned App and its fields. A `none` result
enters the else-branch without constructing an App there.

This pattern is supported by the Compiled target. The focused
[App-owner regression](../compiler/tests/cases/775_if_let_app_owner/Main.rae)
checks nested `mod` calls over multiple frames and exact-once resource cleanup
on normal branch exit, early return and constructor failure. The GPU library
operations shown above remain proposed; that ownership behavior is tested today.

The texture ID itself does not own or release the allocation. At scope exit,
App field cleanup reaches `RenderSystem`, then its `GpuResources` destructor,
which performs the native teardown protocol. App and RenderSystem need no custom
destructors just to drop their fields, and no custom copies are provided for these
resource-owning types. Partial App creation also cleans up the owners already
created; there is no orphan manager on the failure path.

For a larger scene, the App can also own a `WaterSystem`. Pass the renderer's
resource manager explicitly to the water operations, while water stores its own
CPU data and resource IDs. Multiple water instances can use separate resource
groups in that same manager. No system stores a borrowed reference to another
system, and no hidden current-manager getter supplies state.

## Owners, IDs and copies

| Value | Ordinary copy | Cleanup |
| --- | --- | --- |
| CPU pixels, descriptor, FFT snapshot | Independent owned data | Normal field/container cleanup |
| `TextureId` or `BufferId` | Same identity value naming a separate object | No native release |
| Native owner with `drop` and no `copy` | Compile error under the shipped lifecycle rules | Its destructor releases its resource |
| App containing such an owner | Cannot copy the owner through an enclosing aggregate | Fields drop under the existing rules |

```rae
let first: TextureId = texture
let second: TextureId = first
```

Both IDs refer to the same texture, just as copied entity IDs refer to the same
entity. This is not a deep copy of the texture; the value being copied is an ID.

A GPU manager should define no `copy`: duplicating a live device, queued work and
all resources is not a sensible implicit operation. A texture allocation is also
usually inappropriate for `copy`, which must return a new independent `T`.
Instead, provide an explicit fallible operation:

```rae
func duplicateTexture(resources: mod GpuResources, source: view TextureId) ret opt TextureId
```

It creates another texture and records the supported data copy. Its API specifies
failure and when the result can be used. Native `retain` is not an implementation
of independent value copying: it keeps the same native object alive.

Rae cannot prove that arbitrary C code inside a user `copy` actually creates
independent native contents. That remains part of the function's contract. Do
not confuse calling the copy hook with proving every foreign resource invariant.

## The small remaining language proposal: mark raw pointer operations

Recommend an `unsafe` statement block and an `unsafe` function property. Their
exact semantics need approval. Unlike a module permission scheme, this is local
source code that any app or library can write under the same rules.

The key distinction is between **carrying an owner value** and **accessing its
native representation**. Owning, borrowing and transferring an entire native
wrapper use ordinary Rae. Reading its raw pointer is an explicitly low-level act,
regardless of which module does it.

```rae
type NativeBuffer {
  handle: Ptr
  byteCount: Int
}

# No copy function: this type has a destructor and cannot be copied.
func drop(this: mod NativeBuffer) {
  unsafe {
    releaseNativeBuffer(handle: this.handle)
  }
}
```

`unsafe` is proposed syntax. `releaseNativeBuffer` is the binding operation whose
contract requires a live allocation owned by this wrapper. It is not a new
renderer-specific compiler helper.

Outside an unsafe block, these operations would be rejected:

```rae
let handle: Ptr = buffer.handle
let forged: NativeBuffer = { handle: other.handle, byteCount: 64 }
```

The first reads raw native representation. The second reads and installs it into
another wrapper, bypassing the ownership protocol. Ordinary `let other:
NativeBuffer = buffer` is already rejected by the destructor-without-copy rule.

Inside an unsafe block, a library author can inspect or construct native
representation and is responsible for validity. Unsafe code can create a bad
wrapper or corrupt native memory; it does not acquire permission to bypass Rae's
ordinary copy rules or store a Rae borrow past its lifetime.

This is deliberately less than general field privacy. Ordinary fields such as
`byteCount` remain accessible. Therefore a safe wrapper must not rely on mutable
public metadata as its only memory-safety check. For example, a safe buffer write
must check the allocation's authoritative extent at the native boundary, or the
raw field must identify an internal allocation record carrying that extent.
Changing a public cached size must produce an error, not an out-of-bounds access.
If a real wrapper cannot enforce its invariants this way, bring that example to
review before adding a broader representation-hiding feature.

### Proposed rules to prove with one wrapper

1. Raw pointer field reads/writes, initialization (including defaults), casts,
   address extraction, arithmetic, dereference and unchecked foreign calls
   require explicit unsafe code. A function exposing raw pointer arguments or
   results carries that obligation in its signature.
2. Safe code can hold and borrow a whole owner returned by a safe factory. It can
   transfer owners into fields or supported containers without exposing their
   pointer fields. Generated lifecycle code follows the checked copy/drop rules;
   a user cannot obtain that privilege by using a generic function or reflection.
3. Reflection, formatting, serialization, generic accessors and callable aliases
   must not turn a raw pointer into an ordinary accessible value or create an
   owning bitwise copy. Audit these paths rather than assuming field syntax is
   the only way to reach native representation.
4. Every raw extern call initially requires an unsafe site. An ordinary safe Rae
   wrapper can satisfy its contract. A function marked unsafe requires an unsafe
   call site; its obligations must survive imports and callable conversions.
   If the current callable representation cannot preserve them, reject that
   conversion until it can. Do not invent an erased safe callback workaround.
5. `unsafe` does not allow stored `view`/`mod` references, silent owner copies or
   implicit lifetime extension. An asynchronous C callback owns stable native
   state and must not retain a pointer into movable Rae storage.

Marking every raw extern call is a conservative starting point, including calls
without pointers. Before implementation, review a complete user-authored binding
with these rules and verify that the same source works outside the standard
library. No path allowlist, registration file or special compiler type name is
part of the proposal.

## Use readback as the first real owner

The shipped `webgpu/Readback` bridge already has request-owned native state,
nonblocking polling and safe late-callback cancellation. Wrap that existing
protocol with the shipped lifecycle functions before migrating the whole renderer.

```rae
type ReadRequest {
  handle: Ptr
}

func drop(this: mod ReadRequest) {
  unsafe {
    this.handle = Readback.releaseRead(request: this.handle)
  }
}
```

This is an illustrative wrapper over the existing bridge; the import and unsafe
marking of the foreign declarations belong in the actual implementation. `Ptr`
struct-field lowering must first be fixed. No custom copy function is provided.
An explicit `request.drop()` consumes the request; scope exit otherwise drops it.
Do not redefine `drop` as a reusable close operation.

A factory returns `opt ReadRequest` after starting a bounded map on a manager-owned
buffer. It must validate bounds and mapping state and mark the buffer unavailable
for conflicting use until unmapping completes. A pointer wrapper with a destructor
alone does not enforce that buffer protocol. Keep destination addresses out of
pending request state; copy completed bytes during a temporary destination borrow.

Required first tests: allocation failure, transfer, scope-exit and explicit drop,
repeated requests, cancellation before/after completion, two independent requests,
no double release, and no retained pointer into a growing List. Reuse the existing
native bridge tests and add lifecycle checks around the Rae wrapper.

This pilot proves that the new lifecycle functions help real GPU code. It does
not by itself remove global device state or finish the public GPU resource API.

## GPU ownership stays a library problem

`GpuResources` owns native objects, bookkeeping, groups and pending operations.
The first public resource values are typed IDs; the internal representation can
use lifecycle-managed native wrappers once their lowering and pointer boundary
work. Prefer named fields and lists to numeric global C slots.

A safe native wrapper does not imply that releasing it at any time is valid for
a render operation. Track resource uses from command recording through submission
and completion, including dependencies reached through bind groups and views.
Dropping an unsubmitted recording releases its holds. Retiring a resource rejects
new uses immediately while preserving existing uses until they complete.

Native reference retention may implement those internal holds. It is separate
from ordinary Rae value copying and does not justify a `copy` that merely aliases
an owning texture or manager.

Normal polling never waits for GPU completion. Bounded staging pools defer new
readback when full. Explicit shutdown may wait or transfer native completion
ownership safely. A timeout never justifies freeing callback-reachable memory.
The manager destructor can see all its fields before field cleanup; use the
shipped order instead of inventing another destruction mechanism.

A `WaterSystem` holding only resource IDs must not get a destructor that needs an
unstored borrow of some other App field to release them. Initially the manager
owns water groups, explicit water teardown retires a group through the manager,
and manager drop is the cleanup backstop. Copying such a controller copies CPU
data and IDs; it does not create an independent simulation or cleanup owner.
Create independent water instances through separate factory calls.

Preserve FFT tiers, modulo cascade scheduling, real elapsed wave time, per-cascade
sample times and sea-state generations. A native resource ID being valid does
not make an old readback valid for a rebuilt ocean. Preserve the existing optional
FFT query and Gerstner behavior.

## Resource identity: requirements before representation

A buffer/texture ID must identify the correct manager, resource kind, slot and
live generation. Check before native access in release builds; check byte ranges
without overflow. Releasing/reusing a slot must not silently revive an old ID.
IDs name resources and are not an anti-forgery security boundary.

Do not freeze a 256-bit nonce into the public design now. It was one candidate,
not a language feature or an approved requirement. Compare these implementation
choices using the manager pilot:

| Choice | Useful property | What must be acknowledged |
| --- | --- | --- |
| Manager-local slot and generation only | Small, deterministic within one manager | Cannot distinguish two managers with matching slot/generation |
| Explicitly shared ID issuer for participating managers | Deterministic namespace within that issuer | Adds creation plumbing; independent issuers still need separation |
| Random per-manager namespace | Ordinary independent constructors; no shared issuer | Probabilistic collision guarantee, entropy failure and ID-size cost |
| Retained immutable identity token | Prevents identity allocation reuse while old tokens survive | Extra lifetime/copy machinery; must fit Rae's value-copy contract |

A reusable native address alone is not sufficient. Choose only after demonstrating
two managers in separate Apps, matching slots/generations, teardown/recreation and
exhaustion. State whether the guarantee is deterministic or probabilistic and
measure the cost. Do not smuggle in a global current manager or change `main` to
solve this implementation choice. It remains a review prerequisite for the
multi-manager safe API, not a claim solved by lifecycle hooks.

## Practical work order

1. Treat the shipped create/drop/copy design and stored-borrow diagnostic as the
   baseline. Reconcile old implementation tasks before running them; do not add
   ownership properties or re-ban supported owner containers.
2. Fix necessary existing Ptr C-lowering bugs. This repairs interop; it does not
   authorize exposing pointers throughout application state.
3. Prove the small read-request owner using normal create/drop and no copy. If
   done before unsafe support, label it an internal lifecycle pilot, not a safe
   interop guarantee. Keep explicit shutdown where its protocol is still needed.
4. Review and implement the minimal pointer-access/foreign-call boundary with
   full source examples, especially metadata mutation and generic/reflection
   escape attempts. Test the same library authored outside stdlib.
5. Build the checked manager and settle its identity representation, then migrate
   water, shared 3D, 2D/UI and remaining consumers incrementally.

Keep generated WebGPU bindings and generic callback glue. No new GPU backend,
renderer-specific C policy or unrelated water feature belongs in this work.
Follow the [C-surface gate](webgpu-c-surface-audit.md). Compiler suites use
`compiler/tools/watch-tests.sh` with explicit timeouts and one run at a time;
hardware checks also use explicit timeouts. Preserve maintained example gates
and measure CPU/GPU costs under matched settings.

## Decisions still requiring approval

The remaining proposed language feature is explicit unsafe operations/functions,
with raw pointer access itself carrying the boundary rather than module privilege
or a new owner property. Exact operation, foreign-call and callable rules require
review; the snippets above are not syntax approval.

The GPU library must also select a concrete manager-identity strategy and validate
asynchronous ownership. These are separate decisions. This document changes no
compiler/runtime code, and does not claim lifecycle hooks alone prove native
memory safety. Its purpose is to build on what Rae now has with the smallest
additional mechanism that a real example demonstrates it needs.
