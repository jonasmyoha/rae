# Pointer and GPU interop over Rae lifecycle values

Status: **revision 7 approved contract, 2026-09-10. The syntax and source-file
migration checks are implemented; repository-wide enforcement follows after
the recorded legacy consumers migrate.**

This revision starts from the shipped `create`, `drop` and `copy` behavior and
the working `webgpu/ReadRequest` pilot. It adds one approved language boundary:
raw pointer operations and foreign calls are explicit `unsafe` code.
It does not add another ownership system.

## Recommendation

Use these two spellings:

```rae
unsafe {
  # individual raw operations are allowed in this lexical block
}

func adoptNativeBuffer(handle: copy Ptr, byteCount: view Int) unsafe ret NativeBuffer {
  # callers must accept this function's documented native preconditions
}
```

Keep the existing spelling for foreign declarations:

```rae
func nativeBufferCreate(byteCount: view Int) unsafe extern("example_buffer_create") ret Ptr
```

Every extern declaration must include `unsafe`; omitting it is a compiler error.
The two modifiers say different things. `extern("...")` selects a foreign symbol
and ABI. `unsafe` says callers must accept obligations Rae cannot prove. An
unsafe Rae function has `unsafe` in the same post-parameter modifier sequence
without `extern`. Calling either form requires an `unsafe { ... }` block.

An unsafe function body does not become an unmarked region. Raw operations in
its implementation still appear inside `unsafe { ... }`. This keeps the two
responsibilities visible:

- the post-parameter `unsafe` modifier tells the caller that the function accepts
  an external obligation.
- `unsafe { ... }` shows where an implementation relies on that obligation.

An unsafe block has ordinary block control flow and cleanup. `ret`, `break` and
scope exit behave exactly as they do in any other block; unsafe creates no new
lifetime or cleanup exception.

Any user module may declare explicitly unsafe externs, unsafe Rae functions and
unsafe blocks. There
is no trusted-module list, package registration, stdlib privilege, special
`main` parameter, opaque type modifier or noncopyable modifier.

Unsafe code does not disable Rae's ownership rules. It cannot make a type with
`drop` and no `copy` copyable, revive a consumed binding, store a `view`/`mod`
reference, silently move a value or suppress cleanup.

## What is already Rae today

[Constructors, destructors and copies](constructors-and-destructors.md) is the
implemented lifecycle contract:

- `create` is selected by the expected result type.
- A correctly shaped `drop(this: mod T)` runs once for each owned value.
- Explicit `value.drop()` consumes the value.
- A correctly shaped `copy(this: view T) ret T` creates an independent value.
- A type with `drop` and no `copy` cannot be copied.
- Owner fields, optionals, returns and supported containers recursively move and
  clean up their values.
- `if let app: App = App.create()` directly owns the successful payload and
  drops it when the branch ends.
- Struct copying remains a full value copy. Heap-owning fields use their copy
  operations. A resource ID copies its identity bits because those bits are the
  complete value of the ID.
- Stored `view` and `mod` fields are rejected.

The [resource lifetime example](../examples/120_resource_lifetimes/Main.rae)
demonstrates these rules for native audio slots. The
[App owner regression](../compiler/tests/cases/775_if_let_app_owner/Main.rae)
proves direct optional ownership, nested fields, repeated `mod` frame calls,
early returns and partial-construction cleanup.

The boundary below does not alter any of those rules.

## Readback copy hardening (#897)

The pilot's collection-copy surface was hardened before manager integration:

- `ReadRequest.copyToBytes(destination: mod List(UInt8))` is the SAFE surface.
  A byte has no representation to corrupt, and the native copy is bounded by the
  destination's REAL allocation size (from the allocator), which no caller field
  can inflate, so a forged `length`/`cap` cannot overflow the heap.
- `ReadRequest.copyTo(T, destination: mod List(T))` is the UNSAFE raw reinterpret
  protocol. It memcpys raw GPU bytes over the in-memory representation of the
  elements, which the compiler cannot prove matches (T may be `String`, `Ptr` or
  a destructor-bearing owner whose reps it would corrupt), so the caller takes
  that obligation in an `unsafe { ... }` block.
- `ReadRequest.create(buffer: copy Ptr, range)` is `unsafe`: it adopts a raw
  buffer whose map validity and lifetime the caller must guarantee.
- The native copy `rae_wgpu_read_copy` refuses any request larger than the
  destination's real allocation, so a forged capacity is contained rather than
  overflowing. The request's own recorded byte count is used, never a
  caller-modified mirror. Callback-owned state, exact-once cleanup, cancellation
  and the no-retained-List-pointer property are unchanged.

Enforcing the raw protocol's `unsafe` marker required closing a gap: a call to a
GENERIC unsafe function left the call unresolved in sema (the backend resolves
generic calls by name), so the unsafe-call check did not fire. It now resolves
the callee by name when every visible same-name generic overload is unsafe.

Introducing the first `List(UInt8)` also uncovered a latent backend mangling
bug: the type mangler passed a resolved integer TypeInfo's C spelling
(`uint8_t`, from `rae_int_c_name`) through the `rae_`-prefix fallback, producing
an undeclared `rae_uint8_t` and a second, clashing `List` monomorphization. Only
`int64_t` was whitelisted, so `List(Int)` worked by luck while the narrower
widths would have miscompiled. The mangler now recognises every C scalar
spelling and passes it through verbatim.

## What the shipped readback pilot proves

`webgpu/ReadRequest` is an ordinary Rae type with a raw native request field,
metadata, `create`, `drop` and no `copy`. The hardware and deterministic tests
prove:

- optional construction failure;
- transfer through a returned owner and an `own` parameter;
- automatic branch cleanup and explicit consuming drop;
- cleanup through nested owner fields and `List(ReadRequest)`;
- two independent requests and repeated map/unmap;
- a destination List may grow while mapping is pending;
- cancellation callbacks may arrive after the Rae owner is gone; and
- native request and buffer references are released exactly once.

The native bridge owns stable callback state. It never retains a pointer into a
Rae List. This is the correct asynchronous lifetime protocol.

The pilot now uses the boundary: `ReadRequest` keeps `nativeRequest: Ptr` behind
local unsafe blocks, all bridge declarations explicitly say `unsafe extern`,
and `copyTo` accepts `mod List(T)` rather than a public destination Ptr. The
native bridge still borrows the List data only during the completed copy and
never retains it. Lifecycle behavior is unchanged.

## Interop boundary audit (#896)

The staged boundary from #868/#891 was audited against resolved AST paths. It is
STAGED: the checks run only in a source file that opted in by containing an
`unsafe { ... }` block or an unsafe (non-extern) Rae function; a file with none
gets no pointer checks until the final unconditional pass (#877).

Caught in an opted-in file, outside `unsafe`:

- calling an extern or an unsafe Rae function;
- reading, writing, copying or producing a bare `Ptr` or `opt Ptr` value
  (identifier, field, index, call or method-call result);
- constructing a Ptr-containing value with a struct literal;
- default-constructing raw Ptr storage — a bare `Ptr` field, and transitively an
  `Array` of a Ptr-containing element (an `opt` defaults to `none` and an empty
  `List` has no element, so both stay safe);
- erasing a Ptr-containing value into `Any` with an explicit `box`;
- exposing a Ptr-containing value through `.toJson()` / `.toString()`, and — added
  by this audit — through string interpolation `"{owner}"` and generated
  deserialization `T.fromJson(...)` (deserialization null-constructs the Ptr field).

Also enforced by this audit, independent of the opt-in: an explicitly `unsafe`
extern written with the modifiers in the wrong order (`... extern(...) unsafe`)
is rejected even in a declaration-only bindings file that never opens an unsafe
block. `as Ptr` is separately unavailable (only numeric casts exist), so a raw
pointer cannot be manufactured by a cast in any code.

Staged/missing, deferred to the final unconditional pass (#877) and tracked as
#898 because a correct fix needs the move-versus-copy distinction and must not
over-restrict legal whole-owner moves:

- a whole-value COPY of a raw-Ptr aggregate that has no destructor — `let ys:
  List(Ptr) = xs`, `Array(Ptr, ...)` and a plain `struct { p: Ptr }` copied from
  a place — deep-copies the raw pointer silently. Construction (struct literal,
  default) and copies of destructor-bearing owners are already caught; only the
  no-destructor place-copy slips through. It duplicates an address, not memory,
  and reaches valid C, so it is a contract inconsistency rather than a
  memory-safety escape.
- implicit `Any` boxing at a `ret`/assignment whose target type is `Any` (only an
  explicit `box` operand is checked today).

Neither reaches invalid generated C; both are pointer-visibility contract gaps.

## Exact pointer rules

`Ptr` remains a primitive for an untyped foreign address. It never owns the
allocation it names. Copying a Ptr copies an address, not the foreign object.

The following source operations require an enclosing unsafe block:

1. Creating a Ptr value, including a cast, null/default initialization, native
   address extraction or an extern result.
2. Reading, writing, comparing, copying or moving a Ptr value.
3. Reading or writing a struct field whose declared type is Ptr.
4. Constructing or default-initializing a struct payload that initializes a Ptr
   field, including an omitted default for that field.
5. Reading or writing a `List(Ptr)`, `Array(Ptr, ...)`, `opt Ptr` or generic
   specialization at the point where it produces, consumes or copies a Ptr.
6. Pointer casts, arithmetic and dereference. Pointer arithmetic and dereference
   should remain unavailable until a concrete API separately justifies their
   spelling; `unsafe` does not make an unimplemented operator exist.
7. Calling an extern function or an unsafe Rae function.
8. Forming, installing or reading a raw callback address or userdata Ptr.

Declaring a type with a Ptr field is allowed in any module. Declaring an extern
is also allowed. The boundary applies when code manipulates the value or crosses
the ABI.

A call whose argument or result is directly `Ptr` requires unsafe even if the
called Rae function is not declared unsafe, because the call itself produces or
consumes a raw pointer. Add `unsafe` after the parameter list as well when the
function delegates additional validity, ownership, alignment or lifetime
obligations to callers.
A safe factory can return a wrapper containing Ptr because the caller receives
the whole wrapper rather than its raw field.

Safe code may perform these whole-value operations without seeing the Ptr:

- own a wrapper returned by a safe constructor;
- borrow the whole wrapper as `view T` or `mod T`;
- call its safe methods;
- move it with `own` into a field, return, optional or supported container;
- let normal scope cleanup call its destructor; and
- read or modify ordinary public metadata fields.

These operations are safe because the compiler moves or borrows the complete
value according to the shipped lifecycle contract. They do not project the raw
field or manufacture a second owner.

Compiler-generated moved-from bookkeeping and cleanup dispatch are not source
pointer operations. The compiler may use null internally while implementing a
move, but source code cannot obtain that Ptr without an unsafe site.

### Defaults and partial values

This is rejected in safe code because it constructs a Ptr field:

```rae
let buffer: NativeBuffer
```

It is allowed inside unsafe code only when `NativeBuffer.drop` accepts the null
default and the programmer accepts that contract. Safe code instead calls the
factory:

```rae
if let buffer: NativeBuffer = NativeBuffer.create(byteCount: 4096) {
  useBuffer(buffer: buffer)
}
```

`none` does not construct an optional payload, so an empty
`opt NativeBuffer` is safe. Creating an empty `List(NativeBuffer)` is safe
because no element exists yet. Moving a constructed owner into that List is
safe. Default-constructing an Array of pointer wrappers would construct every
element and therefore requires unsafe code unless an ordinary safe constructor
builds valid elements.

This is an operation rule, not a hidden property attached to `NativeBuffer`.

## Complete user-authored native buffer module

The following is the proposed complete shape of a module outside stdlib. The C
side stores the authoritative allocation size and makes release of null harmless.
`nativeBufferWriteByteChecked` validates the live handle, checks the index using
overflow-safe arithmetic and performs no write on failure.

```rae
# nativeBuffer/NativeBuffer.rae

type NativeBuffer {
  handle: Ptr
  byteCount: Int
}

# Every extern declaration explicitly states its unsafe caller obligation.
func nativeBufferCreate(byteCount: view Int) unsafe extern("example_buffer_create") ret Ptr
func nativeBufferConfigure(handle: copy Ptr) unsafe extern("example_buffer_configure") ret Bool
func nativeBufferRelease(handle: copy Ptr) unsafe extern("example_buffer_release") ret Ptr
func nativeBufferWriteByteChecked(
  handle: copy Ptr,
  index: view Int,
  value: view UInt8
) unsafe extern("example_buffer_write_byte_checked") ret Bool

# Safe factory. Failure before the return releases every acquired native value.
func create(byteCount: view Int) ret opt NativeBuffer {
  if byteCount < 1 {
    ret none
  }
  unsafe {
    let handle: Ptr = nativeBufferCreate(byteCount: byteCount)
    if not nativeBufferConfigure(handle: handle) {
      nativeBufferRelease(handle: handle)
      ret none
    }
    ret NativeBuffer { handle: handle, byteCount: byteCount }
  }
}

# Safe borrowed operation. Public metadata is deliberately not used as a bound.
func writeByte(
  this: mod NativeBuffer,
  index: view Int,
  value: view UInt8
) ret Bool {
  if index < 0 {
    ret false
  }
  unsafe {
    ret nativeBufferWriteByteChecked(
      handle: this.handle,
      index: index,
      value: value
    )
  }
}

# Safe destructor around a null-safe native release. There is no copy function.
func drop(this: mod NativeBuffer) {
  unsafe {
    this.handle = nativeBufferRelease(handle: this.handle)
  }
}

# Adoption cannot verify unique ownership, so its caller accepts that obligation.
func adoptNativeBuffer(
  handle: copy Ptr,
  byteCount: view Int
) unsafe ret NativeBuffer {
  unsafe {
    ret NativeBuffer { handle: handle, byteCount: byteCount }
  }
}

# Returning an existing whole owner is an ordinary move.
func passThrough(buffer: own NativeBuffer) ret NativeBuffer {
  ret buffer
}
```

`create` is safe because it accepts ordinary values, validates them, confines
the foreign work to an unsafe block and returns either one valid owner or none.
If configure fails, the local raw handle is released before returning. Once the
struct is constructed, every path is covered by Rae cleanup.

`writeByte` is safe even if application code changes `buffer.byteCount`. It does
not pass that public field as the authoritative bound. The native allocation
record validates the actual handle and size:

```rae
if let buffer: NativeBuffer = NativeBuffer.create(byteCount: 16) {
  buffer.byteCount = 1000000
  let wrote: Bool = writeByte(this: buffer, index: 999999, value: 7 as UInt8)
  # wrote is false; no out-of-bounds access occurs.
}
```

Public metadata may affect display, scheduling or an early conservative reject.
It must never enlarge the range accepted by a safe memory operation. A pure Rae
GPU manager follows the same rule: validate an ID against its authoritative
manager slot, generation, usage and allocation size.

### Ordinary owner use

```rae
func consume(buffer: own NativeBuffer) ret Bool {
  # buffer drops on return.
  ret writeByte(this: buffer, index: 0, value: 42 as UInt8)
}

func demonstrateOwners() ret Bool {
  let buffers: List(NativeBuffer) = List.create(NativeBuffer, cap: 2)

  if let first: NativeBuffer = NativeBuffer.create(byteCount: 16) {
    buffers.add(value: own first)
  } else {
    ret false
  }

  if let second: NativeBuffer = NativeBuffer.create(byteCount: 32) {
    let returned: NativeBuffer = passThrough(buffer: own second)
    if not consume(buffer: own returned) {
      ret false
    }
  } else {
    ret false
  }

  # The List drops its first NativeBuffer here.
  ret true
}

func explicitCancellation() ret Bool {
  if let buffer: NativeBuffer = NativeBuffer.create(byteCount: 64) {
    buffer.drop()
    # buffer is consumed and cannot be used again.
    ret true
  }
  ret false
}
```

No second mutable owner binding is needed after `if let`. The branch binding is
the owner and can be borrowed as `mod`. The List and optional behavior are the
already shipped behavior, not permissions granted by unsafe code.

For this owner, early close is spelled `buffer.drop()` and consumes the value.
There is no reusable `close` state. A different API may define an ordinary
`close(this: mod T)` transition when callers must retain and inspect a closed
value; its `drop` must still be a harmless cleanup backstop. That is a library
state machine, not another language lifecycle hook.

An infallible independent native copy could use the shipped
`func copy(this: view T) ret T` hook. This buffer deliberately has none because
allocation and device copies can fail. A named fallible operation is clearer:

```rae
func duplicateBuffer(source: view NativeBuffer) ret opt NativeBuffer
```

It must allocate independent native storage. Retaining or aliasing the same
handle is not a deep copy. An App or manager containing `NativeBuffer` therefore
also cannot be copied through its fields.

## Rejected duplication and representation access

Given a live `first: NativeBuffer`, safe code gets these diagnostics:

```rae
let second: NativeBuffer = first
# error: NativeBuffer has drop but no copy; move it with own or define a real copy

let handle: Ptr = first.handle
# error: reading a Ptr field requires unsafe { ... }

first.handle = otherHandle
# error: writing a Ptr field requires unsafe { ... }

let forged: NativeBuffer = { handle: otherHandle, byteCount: 64 }
# error: initializing a Ptr field requires unsafe { ... }

let empty: NativeBuffer
# error: default-initializing a Ptr field requires unsafe { ... }
```

The first error remains inside an unsafe block. Unsafe authorizes the pointer
operations it encloses; it does not bypass the missing copy function. A library
author can intentionally forge a wrapper inside unsafe code, just as C can. That
site carries responsibility for unique ownership, validity and eventual drop.

The following raw adoption is explicit at both levels:

```rae
unsafe {
  let raw: Ptr = obtainPointerSomehow()
  let buffer: NativeBuffer = adoptNativeBuffer(handle: raw, byteCount: 64)
  useBuffer(buffer: buffer)
}
```

`adoptNativeBuffer` is unsafe because no implementation can infer whether
`raw` is live, uniquely transferred, correctly typed or owned by the matching
foreign allocator.

## Extern and unsafe-function obligations

An unsafe block means the author has checked every documented precondition for
the enclosed operation. It is not a general request to ignore diagnostics.

For an extern call, the author is responsible for:

- matching the declared C ABI, including widths, layout and calling convention;
- passing live pointers with the required alignment and access mode;
- keeping pointed-to storage alive for the full time C may use it;
- respecting thread, context and reentrancy constraints;
- checking return/status values before using outputs; and
- following the foreign ownership and release protocol exactly once.

For a function with the post-parameter `unsafe` modifier, its documentation
states the subset delegated to the caller. The implementation remains
responsible for everything else. A safe Rae function may use unsafe internally
only when it establishes all preconditions it hides from its caller.

All extern declarations explicitly include `unsafe`, and all their calls require
an unsafe block, including calls whose visible signature contains only numbers.
C implementation behavior and ABI agreement are outside Rae's type proof.
Rather than add a safe-extern escape hatch or privileged declarations, expose
an ordinary safe Rae wrapper when a foreign operation has a safe contract.

## Raw callbacks and future callables

Rae currently has no first-class function values or Rae-to-C callback syntax.
This proposal does not add either feature. Existing WebGPU callbacks remain in
generic C ABI glue with request-owned native state.

A callback stored as Ptr follows the pointer rules. Obtaining the callback
address, placing it in a C callback-info field, reading it back and passing its
userdata are unsafe. The unsafe site must establish:

- the exact C signature and calling convention;
- stable callback and userdata addresses;
- a lifetime extending through a possible late callback;
- thread-safe access or confinement to the documented thread; and
- cancellation that does not free callback-reachable state early.

A pointer into a Rae stack value, List storage or other movable allocation
cannot be retained by an asynchronous callback. The readback bridge is the
model: native request state holds separate owner/callback references, and the
callback may retire its reference after Rae cancellation.

If Rae later gains callable values, unsafe is part of the callable type/effect.
Assigning an unsafe function to a safe callable, returning it through a generic
identity helper or erasing the effect through an alias must be rejected. Calling
an unsafe callable requires an unsafe block. No callable syntax is proposed or
implemented by this work; the non-erasure rule prevents this boundary from
being designed away accidentally later.

## Generics, reflection and serialization

Unsafe cannot disappear through a generic specialization.

Safe generic code may move, borrow and drop a whole `NativeBuffer` because those
operations use the normal lifecycle contract. `List(NativeBuffer).add(value:
own buffer)`, `viewAt` of the whole wrapper and List cleanup remain legal.

A generic operation that produces or consumes Ptr becomes unsafe at the
specialization site. For example, `identity(Ptr, value: raw)` and
`List(Ptr).copyAt(...)` require an unsafe block. The compiler must never infer a
safe specialization merely because the source wrote a generic `T`.

Existing copy rules still govern aggregates:

- copying `NativeBuffer` fails because it has drop and no copy;
- copying a struct containing `NativeBuffer` fails transitively;
- copying a copyable record containing a Ptr is itself a raw Ptr copy and
  requires unsafe code; and
- unsafe does not turn a bitwise copy into an independent native allocation.

Field reflection follows the selected field type. A safe `fields(value)` loop
may process ordinary metadata fields. If it selects a Ptr field, compiling that
specialization reports that raw field access requires unsafe. Binding it as
`any` does not erase the requirement.

Automatic formatting, equality, hashing or serialization must not print,
compare or encode a Ptr through a safe generic path. A reflection serializer
that reaches a Ptr field fails to compile unless its operation and call site are
unsafe. A custom safe serializer can explicitly emit ordinary metadata while
omitting native representation:

```rae
func describe(buffer: view NativeBuffer) ret String {
  ret "NativeBuffer(byteCount={buffer.byteCount})"
}
```

Deserialization cannot construct a pointer owner from an address. A safe decoder
must call a safe factory and create a new allocation. Serializing an address and
later adopting it is an unsafe application protocol, not value persistence.

These checks are based on the concrete operation's resolved type. They do not
need a module allowlist, an owner registry or an opaque/noncopyable property.

## ReadRequest after enforcement

The current owner remains structurally the same:

```rae
type ReadRequest {
  nativeRequest: Ptr
  byteCount: UInt64
}
```

Its implementation gains local markers:

```rae
func drop(this: mod ReadRequest) {
  unsafe {
    this.nativeRequest = Readback.releaseRead(request: this.nativeRequest)
  }
}

func poll(this: view ReadRequest) ret Int {
  unsafe {
    ret Readback.pollRead(request: this.nativeRequest)
  }
}
```

The current `copyTo(... destination: Ptr ...)` remains an unsafe internal
operation because its caller supplies an untyped address and claimed capacity.
The safe public form takes a collection borrow and exposes its address only
inside the wrapper for the duration of the synchronous copy:

```rae
func copyTo(
  this: view ReadRequest,
  destination: mod List(UInt8)
) ret Bool {
  unsafe {
    ret Readback.copyReadChecked(
      request: this.nativeRequest,
      destination: destination.data,
      destinationBytes: destination.length
    ) is 1
  }
}
```

The native request's stored size and the collection's actual initialized byte
length are authoritative. `ReadRequest.byteCount` may be useful metadata, but
changing it cannot enlarge the native copy. The native function retains neither
`destination.data` nor the List. Growth before or after this call remains safe.

This safe overload may require a small ABI adjustment during implementation.
It does not change the callback-owned request state or cancellation protocol.

## Normal App and RenderSystem ownership

The GPU library uses the same lifecycle functions. Typed IDs are copyable
identities; `GpuResources` owns allocations and has `create`/`drop` with no
`copy`. The manager identity representation is proposed in
[gpu-resource-identity-design.md](gpu-resource-identity-design.md) (#892, approved
2026-09-11): a copyable `{managerTag, slot, generation}` value per kind, the tag
drawn from an explicitly shared issuer.

```rae
open gpu/GpuResources

type RenderSystem {
  resources: GpuResources
  waterTexture: TextureId
}

func create(texturePath: view String) ret opt RenderSystem {
  if let resources: GpuResources = GpuResources.create() {
    if let texture: TextureId = loadTexture(
      resources: resources,
      path: texturePath
    ) {
      ret RenderSystem {
        resources: own resources,
        waterTexture: texture
      }
    }
    # resources drops here if texture creation failed.
  }
  ret none
}

func drawFrame(this: mod RenderSystem) {
  drawTexture(
    resources: this.resources,
    texture: this.waterTexture
  )
}

type App {
  renderSystem: RenderSystem
}

func create() ret opt App {
  if let renderSystem: RenderSystem = RenderSystem.create(
    texturePath: "water.png"
  ) {
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
    # app, RenderSystem and GpuResources drop in field order here.
  } else {
    log("App creation failed")
  }
}
```

`gpu/GpuResources` supplies `GpuResources`, `TextureId`, `loadTexture`,
`drawTexture` and `shouldClose`. Their ownership roles are fixed here; the
fields and bit layout of `TextureId` are settled by the #892 identity design (approved 2026-09-11). The
application does not depend on that representation.

There is one persistent ownership tree:

```text
main branch owns App
  App owns RenderSystem
    RenderSystem owns GpuResources
      GpuResources owns native resources and pending-operation state
    RenderSystem stores copyable TextureId values
```

`main` has its normal signature. No runtime resource is injected. Frame calls
borrow the existing App and manager. There is no redundant mutable rebinding
after `if let`, no hidden current manager and no stored borrow between systems.

Copying `TextureId` copies the complete identity value. It does not deep-copy a
texture because the ID does not own that texture. A named fallible operation
such as `duplicateTexture(resources: mod GpuResources, source: view TextureId)`
creates an independent allocation when that behavior is wanted.

Safe manager operations validate the ID against authoritative manager state:
manager identity, kind, live slot, generation, usage and overflow-safe range.
Changing a caller-visible cached field cannot extend access. Resource recording,
submission retirement, manager identity and exhaustion remain library design
work; unsafe syntax does not solve those protocols.

## Implementation and migration boundary

The compiler implementation is staged so existing bindings can migrate without
pretending repository-wide enforcement is complete:

1. **Implemented:** parse `unsafe { ... }` and the post-parameter function modifier, as in
   `func adopt(...) unsafe ret T`; retain the effect on resolved calls.
2. **Implemented for adopted source files:** require every extern declaration to spell the modifier before `extern`, as in
   `func native(...) unsafe extern("symbol") ret T`.
3. **Implemented for adopted source files:** diagnose source Ptr creation, field access, default initialization and copy
   outside unsafe blocks.
4. **Implemented for resolved calls:** preserve unsafe call obligations through qualified calls and every
   compiler-supported alias path.
5. **Implemented at resolved expression sites:** apply the rule after generic substitution and during field reflection,
   formatting and serialization generation.
6. **Implemented:** add the safe ReadRequest collection-copy surface while retaining raw internal
   functions for existing manager protocols.
7. **Inventoried and queued:** migrate current Ptr/extern consumers in bounded tasks. During
   migration, a source file adopts enforcement when it contains an unsafe block
   or unsafe Rae function. This rule is identical for every module and grants no
   package privilege. The inventory found 804 extern declarations across 62
   library/example source files (779 in 41 library files and 25 in 21 example
   files), plus 23 library/example files that mention Ptr. Compiler regression
   fixtures are tracked separately. Generated bindings, non-renderer interop,
   legacy platform surfaces and hand-written GPU consumers have bounded
   migrations; final unconditional enforcement waits for those migrations and
   maintained example gates.

   Migration progress: generated WebGPU bindings adopted the boundary (#893).
   The bounded non-renderer interop surface adopted it too (#894): `core`,
   `String`, `Math`, `Time`, `Io`, `Sys`, `Channel`, `Filesystem`/`Files`, `Image`,
   `Tinyexpr`, `compress/Oracle`, `sys/Spotify`, `HotReload` and the
   `ui/SceneFile` / `Scene3dFile` serialization bridges. Every extern in those
   modules now spells `unsafe extern`; each safe wrapper keeps its ordinary
   signature and wraps its foreign call in the smallest `unsafe { ... }` block,
   while wrappers that hand out or accept a raw pointer without validating it
   (`String.toCStr`/`fromCStr`, `Sys.encrypt`/`decrypt`) are themselves `unsafe`.
   `ui/WindowGeometry` keeps declaration-only marking: it is coupled to the
   `Gpu2d` renderer externs, so its full adoption lands with the GPU consumers
   (#876).

   The legacy window/media boundary adopted the declarations too (#895): every
   foreign declaration in `Raylib` (83), `Sdl3` (14) and `ui/legacyRaylib/Msdf`
   spells `unsafe extern`. These are pure declaration modules (no wrappers of
   their own). Consumer-side `unsafe { ... }` block wrapping in the
   `ui/legacyRaylib/*` shell modules is deferred (#900): those modules are the
   dual-backend raylib/`Gpu2d` shell — they call `Gpu2d` externs directly, so
   their block wrapping is entangled with the GPU-consumer migration (#876), and
   the only examples that exercise them live under `examples/legacy/` which the
   maintained example gate prunes (and which carry pre-existing compile errors),
   so there is no maintained verification path for that wrapping in isolation.
   `#877`'s final sweep enforces the remaining declaration-only files. `Sdl3`'s
   consumers are the maintained raytracer examples, which stay green.

Positive tests must include the same native-buffer module authored outside
stdlib, safe wrapper use, direct `if let` owners, owner fields, returns,
`List(NativeBuffer)`, optional failure and explicit drop.

Negative tests must cover every example in the rejection section, unsafe-effect
erasure through generics/reflection/serialization, unsafe extern calls from safe
code, default Ptr fields, and attempts to call an unsafe function without a
block. Existing owner-container tests remain green.

This work does not add pointer arithmetic, stored borrows, first-class callables,
a GPU backend, a native-owner property or a module permission mechanism.

## Relationship to older proposals

Earlier native-handle drafts proposed registered modules, ownership properties,
noncopyable/opaque declarations and a resource parameter supplied to `main`.
Those mechanisms are withdrawn. The shipped lifecycle contract replaces their
ownership portion. This revision proposes only a local operation/call boundary.

The older WebGPU management notes remain useful for asynchronous native holds,
resource retirement and manager-owned groups. They do not define language
semantics. The separate manager-identity review must choose an ID strategy after
demonstrating two independent Apps, matching slots/generations, recreation and
exhaustion. No fixed nonce, global issuer or hidden manager is approved here.

## Approved contract

The maintainer approved these exact points on 2026-09-10, including the revised
post-parameter spelling and explicit marking of extern declarations:

1. Blocks use `unsafe { ... }`.
2. Unsafe Rae functions use `func name(...) unsafe ret T` or
   `func name(...) unsafe { ... }`; function modifiers stay after `)`.
3. Every extern declaration explicitly includes both distinct modifiers, as in
   `func name(...) unsafe extern("symbol") ret T`. An extern without `unsafe` is
   a compile error. All extern calls require an unsafe block.
4. Unsafe-function bodies still mark their raw operations with unsafe blocks.
5. The eight listed Ptr operations require unsafe, while whole-owner
   borrow/move/return/container/drop operations remain ordinary Rae.
6. Unsafe never bypasses copy, move, drop or stored-borrow diagnostics.
7. Generic/reflection/serialization resolution cannot erase Ptr or unsafe-call
   obligations.
8. There is no callable feature in this implementation; raw callbacks remain C
   ABI Ptr values with explicit unsafe construction and native-owned state.
9. Safe wrappers validate authoritative native/manager state and cannot trust
   publicly mutable metadata to enlarge memory access.
10. Any module may author the same boundary; there is no privileged inventory,
   opacity/noncopyable property or special `main` signature.

The GPU identity and resource-retirement decisions remain separate library
reviews.
