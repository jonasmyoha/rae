# Ptr and GPU resources in Rae

Status: **revision 4, 2026-09-09: concrete example and semantics proposed for
maintainer approval.** The revision 3 direction is accepted; the exact additions
and identity tradeoff below are not yet approved or implemented.

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

This revision proposes exact spelling and behavior through the complete example
below. Approval is still required before compiler changes. The examples use new
properties and statements; they are design examples, not currently runnable Rae.

## What an ordinary app looks like

All code below illustrates proposed APIs. It is not a compilable example of
currently shipped GPU ownership support. The GPU factory returns an optional owner, making context-creation failure
explicit. The native-buffer example below defines the same ownership pattern
in full.

```rae
func runGraphics(initial: own GpuResources) {
  var resources: GpuResources = own initial
  if let texture: TextureId = loadTexture(resources: resources, path: "water.png") {
    drawTexture(resources: resources, texture: texture)
  }
}

func main() {
  if let resources: GpuResources = createGpuResources() {
    runGraphics(initial: resources)
  } else {
    log("GPU creation failed")
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

## Complete native-buffer example

Recommend these source-level additions, using Rae's existing property positions:

```rae
type NativeBuffer: opaque noncopyable drop {
  pointer: Ptr
  byteCount: Int
}
```

- `opaque`: fields and structural construction belong to the defining module.
  Other modules can name, borrow and transfer the value, but cannot access its
  representation. This is not a privileged module list: every module can declare
  its own opaque types. Ordinary types remain cross-file visible as today.
- `noncopyable`: ordinary copy operations are errors, even inside this module
  and inside unsafe code. Transfer still uses existing `own` syntax.
- `drop`: opt this type into one defining-module hook with the exact signature
  `func drop(this: mod NativeBuffer)`. Its body runs before automatic field cleanup.
  Missing, ambiguous or unsafe hooks are declaration errors. Existing functions
  named `drop` on types without the property do not silently become hooks.

The properties are independent, not a magic native-buffer pattern. Their grammar
uses whitespace, matching Rae type properties; no attributes, per-type compiler
registration, or C field-name convention is introduced.

Opacity also blocks default/zero construction, field reflection, generated
serialization and representation casts outside the defining module. Generic code
keeps its definition-site access rights when specialized; importing or instantiating
it does not acquire the owner's module rights. Compiler-generated structural
cleanup can traverse fields without exposing those fields to user code. Public
debug formatting may name the opaque type but must not reveal its pointer.

### Native ABI used by this example

This C file is only the small demonstration library being bound. It is not a new
renderer helper or proposed compiler builtin. All its functions are defined here;
there is no hidden allocator or cleanup service.

```c
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void* demoBufferAllocate(int64_t byteCount) {
    if (byteCount <= 0 || (uint64_t)byteCount > SIZE_MAX) return NULL;
    return calloc((size_t)byteCount, 1);
}

bool demoBufferIsNull(void* pointer) {
    return pointer == NULL;
}

void demoBufferFill(void* pointer, int64_t value, int64_t byteCount) {
    memset(pointer, (unsigned char)value, (size_t)byteCount);
}

void* demoBufferRelease(void* pointer) {
    free(pointer);
    return NULL;
}
```

`demoBufferFill` requires a live writable allocation of at least `byteCount`
bytes, nonnegative count and value 0..255. `demoBufferRelease` accepts null or
one live allocation returned by the allocator, exactly once. Allocation returns
zeroed bytes or null; it retains no borrowed input. These obligations are checked
by the safe wrapper, not established merely by the `extern` declaration.

### The whole Rae wrapper: `nativeBuffer/NativeBuffer.rae`

```rae
func nativeBufferAllocate(byteCount: copy Int) unsafe extern("demoBufferAllocate") ret Ptr
func nativeBufferIsNull(pointer: copy Ptr) unsafe extern("demoBufferIsNull") ret Bool
func nativeBufferFill(pointer: copy Ptr, value: copy Int, byteCount: copy Int) unsafe extern("demoBufferFill")
func nativeBufferRelease(pointer: copy Ptr) unsafe extern("demoBufferRelease") ret Ptr

type NativeBuffer: opaque noncopyable drop {
  pointer: Ptr
  byteCount: Int
}

func createNativeBuffer(byteCount: view Int) ret opt NativeBuffer {
  if byteCount <= 0 { ret none }
  unsafe {
    let pointer: Ptr = nativeBufferAllocate(byteCount: byteCount)
    if nativeBufferIsNull(pointer: pointer) { ret none }
    ret NativeBuffer { pointer: pointer, byteCount: byteCount }
  }
}

func size(this: view NativeBuffer) ret Int {
  ret this.byteCount
}

func fill(this: mod NativeBuffer, value: view Int) ret Bool {
  if value < 0 or value > 255 { ret false }
  if this.byteCount == 0 { ret false }
  unsafe {
    nativeBufferFill(pointer: this.pointer, value: value, byteCount: this.byteCount)
  }
  ret true
}

func close(this: mod NativeBuffer) {
  unsafe {
    this.pointer = nativeBufferRelease(pointer: this.pointer)
  }
  this.byteCount = 0
}

func drop(this: mod NativeBuffer) {
  close(this: this)
}
```

The constructor immediately wraps a successful allocation before any later
fallible work. Raw `Ptr` locals have no automatic ownership: inserting another
failure path between allocation and construction would require explicit cleanup.
This obligation remains the unsafe author's responsibility. The returned opaque
owner hides its pointer; callers receive no borrowed native address.

`fill` is safe because outsiders cannot replace the pointer/count, construction
makes them consistent, and `close` leaves a null pointer and zero count. A mutable
borrow is required for mutation of the native contents. All operations are
synchronous here, retain no borrowed address, and use the same thread. A real
asynchronous wrapper needs the additional callback rules described below.

`close` is intentionally idempotent. Closing twice calls `free(NULL)` the second
time. Closing then leaving scope is also valid; there is one release of the live
allocation. `size` after close returns zero and `fill` returns false. There is no
public reset that resurrects the owner.

### A normal application: `Main.rae`

```rae
import nativeBuffer/NativeBuffer

func useBuffer(initial: own NativeBuffer) {
  var buffer: NativeBuffer = own initial
  let filled: Bool = NativeBuffer.fill(this: buffer, value: 7)
  log(filled)
  log(NativeBuffer.size(this: buffer))
  NativeBuffer.close(this: buffer)
  # Scope exit runs drop; the already-closed allocation is not freed twice.
}

func main() {
  if let buffer: NativeBuffer = NativeBuffer.createNativeBuffer(byteCount: 64) {
    useBuffer(initial: buffer)
    # buffer was consumed by the own parameter; it cannot be used again.
  } else {
    log("buffer allocation failed")
  }
}
```

Remove the explicit `close` and scope-exit cleanup still releases the allocation.
The caller uses no unsafe block and no special `main` parameter. Moving `initial`
into `buffer` does not duplicate the pointer's ownership. The caller's produced
optional payload transfers into the `if let` binding, then the `own` call.

### Return, failure and partial construction

These additional functions live in an ordinary application module importing the
wrapper; they use no representation access or unsafe operations:

```rae
type BufferPair {
  first: NativeBuffer
  second: NativeBuffer
}

func createBufferPair(firstSize: view Int, secondSize: view Int) ret opt BufferPair {
  if let first: NativeBuffer = NativeBuffer.createNativeBuffer(byteCount: firstSize) {
    if let second: NativeBuffer = NativeBuffer.createNativeBuffer(byteCount: secondSize) {
      ret BufferPair { first: own first, second: own second }
    }
    # Failure to allocate second leaves first owned here; it drops on exit.
  }
  ret none
}

func passBufferPair(pair: own BufferPair) ret BufferPair {
  ret pair
}

func inspectBufferPair(pair: view BufferPair) ret Int {
  ret NativeBuffer.size(this: pair.first) + NativeBuffer.size(this: pair.second)
}
```

`BufferPair` is structurally noncopyable and needs field cleanup without adding
properties to its declaration. On success both locals move into the returned
pair; neither is dropped in the factory. On second-allocation failure only the
first allocation is released. Passing/returning the whole pair transfers it.
Reading its fields through `view` borrows them; it does not copy either owner.

A constructor that can fail while evaluating field expressions needs initialized
field tracking: release every field successfully initialized before the failure,
exactly once, and never inspect uninitialized storage. This is required even if
the simple factory above avoids that situation by allocating before construction.
This means supported normal early-exit paths; it does not add exception unwinding
or promise cleanup after process abort.

### Concrete rejected operations

Each line below is a separate negative example with the indicated existing
value in scope; these are not additional working application functions:

```rae
let alias: NativeBuffer = buffer                 # ERROR: cannot copy owner
let pairCopy: BufferPair = pair                  # ERROR: owned field is noncopyable
let address: Ptr = buffer.pointer                # ERROR: opaque field outside defining module
let forged: NativeBuffer = { pointer: raw, byteCount: 64 } # ERROR: opaque construction
useBuffer(initial: borrowedBuffer)               # ERROR: cannot consume a view/mod borrow
let element: NativeBuffer = own pair.first       # ERROR: partial owner moves unsupported initially
let self: NativeBuffer = own self                # ERROR: source not initialized
```

`unsafe` at these call sites does not waive opacity, copy checking or borrowed
Rae reference rules. An unsafe author can still corrupt native memory or violate
an ABI contract: this facility is an explicit responsibility boundary, not a
sandbox for malicious native code.

## Exact low-level and ownership rules proposed

### Unsafe operations and callables

Use `unsafe { ... }` as a lexical statement block and `unsafe` as a function
property after its parameters, as shown by the extern declarations. An unsafe
function puts requirements on its caller; a call requires an unsafe block even
inside another unsafe function. Its body does not implicitly become unchecked.
An unsafe block can contain ordinary control flow including `ret`; it does not
change the surrounding function's return or drop behavior.

In the final enforced mode, every raw `extern` declaration is unsafe by default;
require its source to state `unsafe` explicitly for readability. Safe foreign
operations are exposed through a small ordinary Rae wrapper. The first version
has no `safe extern` override. That is conservative and uniform across modules;
#868 supplies staged auditing so existing bindings migrate before enforcement.

Raw pointer construction, extraction, casts, arithmetic, dereference and native
calls require an unsafe block; raw-pointer parameters/returns require unsafe
function signatures. Raw pointer locals and operations are confined to those
blocks. A stored raw pointer field is allowed in an opaque type; its access and
construction still require an unsafe block in the defining module. Generics may
not expose a substituted raw pointer as a safe public value. No stored `view` or
`mod` references are permitted, including inside unsafe or opaque types.

For this first slice, **do not add a first-class unsafe-callable type**. Reject
conversion of unsafe functions to ordinary safe callable values, aliases with
safe signatures, generic callable arguments or fields. An ordinary safe wrapper
may be used as a callable; its implementation must actually discharge the raw
function's obligations rather than merely erase a flag. Calls through an imported
name retain the original unsafe requirement.

A raw C callback/function pointer may be formed/passed only within the explicit
unsafe boundary using the existing binding mechanism. A native pointer has no
safe-callable conversion. The caller must satisfy calling convention, argument
layout, thread affinity and callback lifetime, including possible calls after
cancellation. Do not capture borrowed Rae storage, `List` elements or noncopyable
owners into escaping callbacks. Keep stable native request state with explicit
ownership. Typed unsafe callable values can be a later reviewed extension, not
an implicit gap in this initial contract.

### Copy, transfer and cleanup

- Reject ordinary copies and `copy` parameters for noncopyable types even when
  the source is a temporary or has no later use. `=` remains copy-or-error for
  existing values. A produced owned result is taken directly by its destination.
- Whole owned locals transfer through `own` expressions, declared `own`
  parameters and Rae's existing owned-local return convention. Sources become
  unavailable. A `view`/`mod` borrow never authorizes transfer of its referent.
- Reject partial moves from fields/elements and self-moves in the initial slice.
  Moves are checked across branches/loops: use requires a live value on every
  incoming path, and a loop cannot repeatedly consume it without reinitialization.
- On replacement assignment, evaluate a valid RHS first, then drop the old
  destination once and install the new owner. Failed RHS evaluation leaves the
  old destination owned. Returning a failure value normally still cleans up locals.
- `drop` is an opt-in property. Resolve exactly one safe, non-generic
  defining-module `func drop(this: mod T)` for that concrete declared type in
  the initial slice. It may contain an unsafe block, but cannot consume `this`,
  return a failure value, recursively drop itself or escape a borrow of itself.
  It runs once for each initialized owner that has not transferred out.
- The hook runs before automatic field cleanup. It must not manually invoke
  field hooks: normal generated cleanup owns that responsibility. To explicitly
  close a native field, call its idempotent `close`, not its hook. Direct calls
  to the hook of a `drop`-marked type are rejected; `close` is the public operation.
- Preserve existing ordinary evaluation/cleanup order. This proposal adds only
  hook-before-fields ordering and exact-once initialized-field cleanup; it does
  not specify a new global field/local destruction order. Order-sensitive native
  dependencies belong in the enclosing owner's hook, which sees all fields live.
- Moved-from storage is skipped by cleanup; no requirement that arbitrary native
  owners have a public null/default value is introduced. Compiler liveness flags
  and type-specific closed-state invariants are separate concepts.
- Normal scope exits and failure returns clean up. Process termination/abort
  does not promise destructors. A hook must complete without propagating an
  error; an explicit close/shutdown API may separately report recoverable errors.

`opaque` without `noncopyable` still needs a valid deep copy of all owned contents.
A type carrying `Ptr` cannot silently obtain a default pointee copy; require it
also to be `noncopyable` in this first version. A `drop` property does not magically
supply a native clone operation. Copyable custom-resource types need later design.

### Optional and container boundary

Support direct owned fields, produced owned results and `opt Owner` first. A
stored optional can be borrowed with typed `if let ... => ...`; produced optionals
transfer payload ownership with `if let ... = factory(...)` as above. Transfer a
stored optional as a whole to an owning helper if it needs to yield its payload;
do not silently move a payload out of a borrowed optional. Dropping `none` does
nothing, and an owned present payload drops once.

For the initial implementation, reject storing noncopyable owners (including
indirect enclosing types) in `List`, `Array`, maps, `Any` or ECS component tables.
This is a proposed deliberate implementation limit, not a fundamental ban on such
containers. Later support must define element transfer, removal and cleanup and
prove that existing copy accessors cannot duplicate owners. App/World resources
are direct fields, so the GPU manager does not require owner containers.

Containers of copyable IDs and CPU data remain supported. Store native object
entries under one native context owner initially, with Rae metadata/IDs in lists.
Do not work around the restriction with a public `List(Ptr)`.

## IDs and independent contexts

A resource ID contains a context identity, slot and generation; the manager passed
to an operation must match that context. Keep distinct `TextureId`, `BufferId`,
`TextureViewId`, `SamplerId`, `PipelineId`, `BindGroupId`, `ReadbackId` and
`SubmissionId` types. Byte ranges add checked offset and length to a `BufferId`.

### Practical alternatives

| Strategy | Separate Apps and recreation | Cost or limitation |
| --- | --- | --- |
| Per-context or per-App counters alone | Both first contexts can issue the same ID | Incorrect for cross-owner detection |
| Native context address alone | Allocator can reuse an address after destruction | Incorrect for stale IDs after recreation |
| One explicitly shared factory assigning serials | Deterministic within that factory's lifetime, if every participating App uses it | Extra creation plumbing; independently created factories need another namespace mechanism |
| Retained identity allocation shared by every ID | Allocation cannot be reused while an old ID retains it | Reference-counted identity metadata, non-POD ID copies and longer-lived allocations; needs a separate copy/identity contract |
| Fresh 256-bit context nonce from OS randomness | Independently constructed contexts need no shared factory | Very small but nonzero accidental collision probability; 32 context bytes per ID |

Recommend **256-bit random context nonces for the first GPU library**, subject to
explicit approval of the probabilistic identity guarantee. This is an ordinary
library/platform operation, not a language feature or a compiler-issued identity.
The alternative if deterministic global uniqueness is required is a separately
reviewed shared factory or retained identity scheme, not a special `main` argument.

Obtain 32 bytes from a cryptographically secure OS source for each context
construction. Failure to obtain bytes fails construction; all-zero is reserved
and retried with a finite retry limit. Do not fall back to a clock, address,
App-local counter or weak generator. The test fixture injects bytes explicitly;
production creation does not accept a caller-chosen context nonce.

Use four `UInt64` words for the context nonce, plus `UInt64` slot and generation:
48 bytes per initial resource ID before any additional representation overhead.
Do not transmit these CPU validation fields to shaders. The manager owns its
nonce; ID copies copy all words. No identity registry or shared native ownership
is hidden in the ID copy. Slot may start at zero; generation starts at one.

For independently uniform nonces, the birthday-bound collision probability among
k created contexts is at most k(k-1)/2^257 (about 4.3e-60 for a billion contexts).
This is **not a deterministic guarantee**. If two contexts receive identical
nonces and also have matching slots/generations, this design cannot distinguish
their IDs. Without shared history it cannot reliably detect that collision. The
same caveat applies across destruction/recreation. Do not write a test that
claims forced duplicate nonces are rejected; document that limitation honestly.
IDs are not an anti-forgery security boundary: code able to edit ID fields and
borrow the manager already has authority to request its resources.

### Two Apps, same slot/generation

Proposed API examples use normal constructors; creation can fail:

```rae
func main() {
  if let first: GpuResources = createGpuResources() {
    if let second: GpuResources = createGpuResources() {
      compareContexts(first: first, second: second)
    }
  }
}

func compareContexts(first: own GpuResources, second: own GpuResources) {
  var firstApp: App = { resources: own first }
  var secondApp: App = { resources: own second }
  # Each App can now independently allocate its first texture.
}

type App {
  resources: GpuResources
}
```

Suppose the first App creates texture `(nonceA, slot 0, generation 1)` and the
second creates `(nonceB, slot 0, generation 1)`. Using the first texture with the
second manager fails its nonce comparison before native lookup. Dropping and
recreating the first manager obtains nonceC; old nonceA IDs fail against it,
subject to the explicit collision limitation above. Moving an App preserves its
manager's nonce. This requires no mutable global table or injected runtime root.

Within one context, reuse increments the slot generation. After the maximum
`UInt64` generation is retired, permanently retire that slot rather than wrap.
Apply checked increment/failure to slot allocation and submission sequences too.
Closing a manager invalidates all IDs immediately; it cannot be reopened with the
same identity. Creation is a new operation with a fresh nonce.

Validate owner, kind, bounds, generation, live state, usage and alignment before
native access, in release builds too. Check ranges without overflow. Invalid IDs
fail rather than silently selecting another resource. IDs are execution-local
and must not be restored from serialized files as live resource references.

Use reduced-width counters and predetermined distinct nonces in deterministic
tests to exercise exhaustion, recreation and two independent Apps. Test OS entropy
failure as a constructor failure. Separately document a forced-collision fixture
as a known limitation, not a successful wrong-owner check. This distinction is
part of the approval decision for the nonce approach.

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

1. Approve or revise the complete native-buffer example, exact properties, unsafe
   call rules, initial container limit and probabilistic identity choice above.
   Queue #878 records this review; it does not infer syntax approval.
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

## Approval record

Previously accepted: normal `main`, no privileged module/owner registry, typed
non-owning IDs, explicit resource ownership, deep-copy values, no stored Rae
borrows and a general low-level facility available to any library author.

**Revision 4 approval is pending.** Specifically review:

1. `type NativeBuffer: opaque noncopyable drop`, defining-module representation
   access and `func drop(this: mod NativeBuffer)` as the explicit cleanup hook.
2. `unsafe { ... }` blocks and the function property `unsafe`, raw extern calls
   requiring that boundary, no safe-callable erasure and no privilege by module.
3. The example's transfer/close/failure behavior and initial direct-field/optional
   support, with owner containers/boxing rejected until implemented safely.
4. Fresh 256-bit context nonces, checked generations and the explicitly stated
   probabilistic cross-context guarantee, without hidden globals or a new main API.

Approval of these concrete items enables #867/#868 and the GPU manager tasks.
If any item changes, update its example and dependent queue entries before code.
This task edits documentation only. Examples using proposed syntax cannot yet be
compiled as Rae; document validation must not be reported as compiler test success.
