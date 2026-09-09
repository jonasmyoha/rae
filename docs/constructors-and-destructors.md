# Constructors and destructors in Rae: `create` and `drop`

Status: **design proposal, 2026-09-09, for maintainer approval.** Nothing here is
implemented. This document is deliberately separate from
`ptr-and-gpu-resource-design.md` (#878): it proposes a smaller language surface
for the same underlying need, and section 9 says exactly which parts of #878 it
replaces and which it leaves alone.

## 1. The idea in one paragraph

A constructor is an ordinary free function named **`create`** that returns the
type. A destructor is an ordinary free function named **`drop`** whose first
parameter is `this: mod T`. Both are plain functions written the way every Rae
method is written today (`func size(this: view NativeBuffer) ret Int`). There are
no new keywords, no type properties, no attributes and no registration: the
**name** is the contract, exactly as `this` as a first parameter already is the
contract for member-call sugar. The compiler resolves `create` by the type it is
asked to produce and calls `drop` where it already performs structural cleanup.

```rae
type NativeBuffer {
  pointer: Ptr
  byteCount: Int
}

func create(byteCount: view Int) ret opt NativeBuffer {
  if byteCount <= 0 { ret none }
  let pointer: Ptr = nativeBufferAllocate(byteCount: byteCount)
  if nativeBufferIsNull(pointer: pointer) { ret none }
  ret NativeBuffer { pointer: pointer, byteCount: byteCount }
}

func drop(this: mod NativeBuffer) {
  this.pointer = nativeBufferRelease(pointer: this.pointer)
  this.byteCount = 0
}

func fill(this: mod NativeBuffer, value: view Int) {
  nativeBufferFill(pointer: this.pointer, value: value, byteCount: this.byteCount)
}

func main() {
  if let buffer: NativeBuffer = create(byteCount: 64) {
    buffer.fill(value: 7)
  }                                   # drop(this: buffer) runs here, once
}
```

That is the whole feature from the user's side.

## 2. Why names and not properties

Rae already has two conventions that work purely by name and signature, and
both are the model for this design:

- **Member-call sugar.** `spec/rae.md` §5.3: any function whose first parameter
  is of type `T` can be called as `value.f()`. No `method` keyword, no `impl`
  block. The function's *shape* makes it a method.
- **The container drop hook that exists today.** `compiler/src/c_stmt.c`
  (`find_drop_overload_for`) already looks up a user function literally named
  `drop` whose first parameter's base type matches a container, and emits calls
  to it during structural cleanup. `lib/core/List.rae`, `lib/collections/*.rae`
  and `lib/ui/ecs.rae` all define `func drop(T: type, this: mod X(T))` and the
  compiler already calls them. The hook is real; it is only restricted to
  generic containers.

So the destructor half of this design is not new machinery. It is: *lift the
restriction that a `drop` hook must be on a generic container.* The constructor
half adds one resolution rule for a function named `create`.

Compare the #878 spelling, `type NativeBuffer: opaque noncopyable drop`, which
adds three type properties, a per-property rule set and the statement
"existing functions named `drop` … do not silently become hooks". This design
says the opposite and means it: **a function named `drop` with the hook
signature IS the hook.** A reader who sees `func drop(this: mod Water)` knows
what it is without looking at the type declaration, which is the same reason
`func size(this: view Water)` needs no annotation to be a method.

## 3. `create`: the constructor

### 3.1 Definition

A **constructor** of `T` is any function named `create` whose declared return
type is `T` or `opt T`. It may take any parameters (all with explicit modes, as
every parameter must). Its body builds the value with the normal struct literal;
nothing about construction changes inside the function.

```rae
func create(cap: view Int) ret List(T)                 # generic: T bound by the caller's expected type
func create(surfaceZ: copy Float, extent: copy Float) ret WaterBody
func create(body: view WaterBody) ret WaterSystem
func create(byteCount: view Int) ret opt NativeBuffer   # fallible: none on failure
```

Returning `opt T` is the fallible constructor. There is no other failure channel:
a constructor that cannot fail returns `T`, one that can returns `opt T`, and the
caller uses `if let` as it does for any optional.

### 3.2 Resolution: `create` follows the struct-literal rule

Rae has no return-type overloading and no type inference, so a bare call
`create(...)` needs to know which type it is producing. The rule is the one the
untyped struct literal already has: **`create(...)` is resolved by the expected
type of the position it appears in, and is legal exactly where an untyped struct
literal `{ ... }` is legal.**

```rae
let names: List(String) = create(cap: 4)          # expected type List(String); binds T = String
var water: WaterSystem = create(body: lake)       # expected type WaterSystem
let bed: Material3d = { baseColor: sand, ... }    # the existing literal rule, same positions
```

Where an untyped literal must be typed, `create` must be qualified with the type:

```rae
ret WaterBody.create(surfaceZ: 0.0, extent: 400.0)       # ret has no left-hand type
let pair: BufferPair = { first: NativeBuffer.create(byteCount: 8), ... }   # allowed either way: the field type is known
log(create(byteCount: 8))                                # ERROR: no expected type; write NativeBuffer.create(...)
```

`T.create(...)` is the **type-qualified call**: the type name before the dot
selects the constructor whose return type is `T`/`opt T`. This mirrors
member-call sugar (instance before the dot selects by first-parameter type;
type before the dot selects by return type). Generic types bind their type
arguments from the expected type or from the qualifier:
`List(String).create(cap: 4)`.

Module qualification keeps working as today (`Water.create(...)` when the
module is named `Water`); it is not required and it is not the mechanism.

### 3.3 Rules

1. **At most one `create` per type in scope.** Two visible `create` functions
   returning the same `T` (across `open`ed modules) is an ambiguity error at the
   call. Named factories (`createLakeToon`, `createRiverToon`,
   `createOceanRealistic`) stay ordinary functions with ordinary names; they are
   not constructors in the language sense and are unaffected.
2. **`create` is not required.** A type without one is built with the struct
   literal, as now. A type with one can still be built with the literal (this
   design does not add field privacy; see section 9).
3. **`create` is not special inside.** It is a normal function body. It may call
   other constructors, may return `none`, and its locals drop on early `ret`
   like any function's. Partial construction is therefore already handled by
   the existing rules: an owned local that was created and not moved into the
   returned literal drops on the failure path (see the `BufferPair` example in
   `ptr-and-gpu-resource-design.md`; it works unchanged).
4. **No return-type overloading elsewhere.** The expected-type resolution is
   defined for the name `create` only, as the literal rule is defined for `{ }`
   only. It is not a general feature.

### 3.4 What `create` buys

- One name for "make me one of these" across every type, generic or not,
  fallible or not: `create(cap: 4)` for a list, `create(body: lake)` for a water
  system, `create(byteCount: 64)` for a native buffer. Today it is `createList`,
  `createWaterSystem`, `createNativeBuffer`, `createComponentTable`, and the
  reader has to know each.
- The type is written once, on the left, which is the rule Rae already enforces
  for literals (`let p: Point = { x: 1 }`, never `Point { ... }` twice).
- Tooling and agents can find every type's constructor by name and return type.
- Constructor and destructor become a visible pair in one file: `create` returns
  the resource, `drop` returns it to the system. `createList` / `free` /
  `shutdownAll` / `waterShutdown` / `gbReleaseTargets` become one vocabulary.

## 4. `drop`: the destructor

### 4.1 Definition

A **destructor** of `T` is a function with exactly this shape, declared in the
module that declares `T` (the same file, or a sibling file merged into the same
module):

```rae
func drop(this: mod T)
```

For a generic type the shape is the one the stdlib already uses:

```rae
func drop(T: type, this: mod List(T))
```

No return type. `this` is `mod`, never `own`: the hook cleans up the value in
place and the compiler owns the storage. Anything else named `drop` whose first
parameter is a `T` is a compile error ("`drop` is reserved for the destructor
of T; it must be `func drop(this: mod T)`"). A `drop` on a type from another
module is an error too. That is the entire opt-in: the name plus the shape.

### 4.2 When it runs

The compiler already emits structural cleanup for every value that owns heap
(`docs/drop-semantics-and-defer.md`, Stage 1: scope exit, before every `ret`,
before reassignment, on `break`/`continue`, reverse binding order, `ret ident`
moves instead of dropping). This design changes only the classification and
the per-type body:

- **Classification.** A type is a drop target if it has a `drop` hook, or if any
  field (transitively, through generics and `opt`) is a drop target. Today the
  base case is hard-coded to `String`/`List`/`StringMap`/`IntMap`; it becomes
  "has a hook or contains one" with those stdlib types simply having hooks.
- **Body.** Dropping a value of `T` means: run `T`'s hook if it has one, then
  drop each field that is a drop target, in **reverse declaration order**. Hook
  first, so it sees every field live (a GPU context can flush before its
  buffers are released). The hook must not drop its own fields; the compiler
  does that next. Elements of a `List(T)` drop before the list's storage, which
  is the per-element loop the backend already injects for stdlib containers.
- **Once.** Each initialized value that has not been moved out drops exactly
  once. `own` parameters, `own` fields, `if let` payloads and returned locals
  follow the existing move rules; moved-from storage is skipped.

Nothing runs on process abort or an unhandled panic; that is unchanged and not
promised. `defer` stays a separate statement for caller-side cleanup and is not
a substitute for a destructor (same conclusion as `drop-semantics-and-defer.md`).

### 4.3 Explicit early destruction

`value.drop()` is a normal call of the hook through member-call sugar. The
compiler treats it as **consuming the value**: after `water.drop()`, `water` is
moved-from, any later use is a compile error, and scope exit skips it.

```rae
var water: WaterSystem = create(body: lake)
water.drop()                 # releases now; `water` is consumed
water.step()                 # ERROR: use of dropped value `water`
```

This replaces two things from #878: the "direct calls to the hook are
rejected" rule and the requirement that every owner also expose an idempotent
`close`. A type may still provide `close`/`shutdown` for a partial, resumable
release (stop accepting work but keep the value alive); that is an ordinary
method with no language meaning.

### 4.4 What a hook may do

- Call extern/native release functions, other methods on `this`, and log.
- Read and write its own fields; the fields are live and the compiler drops the
  droppable ones afterwards.
- It may not return a value, take `this` by `own`, move `this` or a field out,
  or store a `view`/`mod` of `this` anywhere (already impossible: Rae has no
  stored borrows).
- It may not fail. A release that can report an error belongs in an explicit
  `close` method that returns the error; the hook's job is best effort.

## 5. Copying a type that has a destructor

The rule falls out rather than being declared: **a value of a type that has a
`drop` hook, or contains one, cannot be copied with `=` or passed by `copy`.**
It moves with `own`, is borrowed with `view`/`mod`, and is produced by `create`
or a literal.

```rae
let alias: NativeBuffer = buffer          # ERROR: NativeBuffer has a destructor and no copy
useBuffer(initial: own buffer)            # ok: transfer
inspect(buffer: buffer)                   # ok: view/mod borrow, as today
```

Why derived and not declared: a hook exists precisely because the compiler
cannot see what the value holds (a native handle, a GPU allocation). It cannot
deep-copy what it cannot see, so silently deep-copying such a struct would
duplicate a handle and free it twice. Structural drop already deep-copies
`List`/`String` on `=` because the compiler *does* know those; a user hook is
the signal that it does not.

If a type wants to be copyable anyway, the same by-name pattern can give it a
copy constructor later: `func copy(this: view T) ret T`, resolved where `=`
would otherwise be an error. That is a natural extension and is intentionally
**not** part of this proposal; today the answer is "own it or borrow it".

Containers follow the value: `List(NativeBuffer)` is legal (elements drop
through the existing per-element loop) and is itself non-copyable. ECS
component tables of such types are legal under the same rule. This is looser
than #878's "reject owners in containers initially", because the element-drop
loop already exists and copy rejection is the only new check.

## 6. Native handles: `Ptr` fields

Today an ordinary struct with a `Ptr` field is a C-lowering bug (#863: an
invalid generated string conversion; `List(Ptr)` emits a nonexistent type),
which is why the water package parks GPU handles in a C slot store
(`rae_gb_water_set/get`, 160 slots). Fixing the lowering is necessary but not
sufficient: a handle in a type without a hook is silently duplicated by `=`
and silently leaked at scope exit. So the rule here is **a `Ptr` field is
allowed in a `c_struct` (the bindings) and in a type that has a `drop` hook;
anywhere else it is a compile error** ("`T` holds a `Ptr` and has no `drop`").
The hook is what makes holding a handle safe: the value cannot be copied
(section 5) and cannot leak (section 4.2). This is the smallest rule that lets
`WaterSystem` own its pipelines and textures directly and deletes the slot
store; #863 becomes a prerequisite of the implementation task.

Whether reading a `Ptr` field or calling an extern requires an `unsafe` block is
the subject of #878/#868 and is orthogonal; this design works with or without
that boundary.

## 7. What changes in the compiler

Each item maps onto machinery that already exists.

| Area | Today | Change |
| --- | --- | --- |
| Hook lookup (`c_stmt.c` `find_drop_overload_for`, `c_backend.c` struct-drop synthesis) | Only generic containers; matched by first-parameter base name | Any type; exact shape `func drop(this: mod T)`; defining-module check; reserved-name diagnostic for other shapes |
| Classification (`ownership.c` `is_drop_target_type`, `type_needs_cascade_drop`) | Hard-coded `String`/`List`/`StringMap`/`IntMap` | "has a hook or contains one"; stdlib types keep their hooks |
| Struct drop body | Fields only | Hook first, then fields in reverse declaration order |
| Explicit `x.drop()` | Ordinary call, value still dropped at scope exit (double release) | Marks the local moved (`local_moved`), later use is a sema error |
| Copy check (sema) | Heap types deep-copy on `=` | `=`/`copy` rejected for hook-bearing types |
| `Ptr` fields | Lowering bug (#863), so avoided | Lowering fixed; allowed in `c_struct` and hook-bearing types, rejected elsewhere |
| `create` resolution (sema) | None | Resolve by expected type in literal positions; `T.create(...)` type-qualified form; ambiguity diagnostic |
| Lists of hook types | Element loop exists for stdlib | Same loop calls the user hook |

The VM target is not touched (deprecated).

## 8. Migration in `lib` and examples

- `createList(T, cap)`, `createStringMap`, `createComponentTable(T)` gain a
  `create` twin; the old names stay one release as aliases, then go.
- `free()` on `List`/`StringMap`/`IntMap` is deleted: it has no call sites, and
  the hook is the destructor. `docs/STDLIB_GUIDE.md`'s "must provide `free()`"
  rule is replaced by "has a `drop` if it holds anything the compiler cannot
  see".
- `waterShutdown` becomes `drop(this: mod WaterSystem)`; the FFT slot releases
  become field drops of owned handle types; the 160-slot store goes.
- `Gbuffer.shutdownAll`, `shadowShutdown`, `skinShutdown`, `deferredShutdown`,
  `gbReleaseTargets` become hooks on the owning renderer/App resources, called
  by scope exit of `App`.
- Examples stop calling any shutdown function at the end of `main`; the App
  local's drop does it.

## 9. Relation to #878 (`ptr-and-gpu-resource-design.md`)

Replaced by this document, if approved:

| #878 element | Here |
| --- | --- |
| `drop` type property + "exactly one defining-module hook" | The hook by name and shape, section 4.1 |
| `noncopyable` type property | Derived: has a destructor ⇒ not copyable, section 5 |
| "Direct calls to the hook are rejected; `close` must be idempotent" | `x.drop()` consumes the value, section 4.3; `close` is optional and ordinary |
| "Reject owners in containers initially" | Allowed; containers inherit non-copyability, section 5 |
| `createNativeBuffer` named factory | `create`, section 3 |

Left alone by this document (separate decisions, not contradicted):

- `opaque` (field privacy). Not needed for cleanup correctness. Whether Rae
  wants module-private fields at all is its own question; this design works
  with every field visible, as all Rae fields are today.
- `unsafe` blocks, `unsafe` extern obligations, callbacks (#868).
- Typed resource IDs, context nonces, GPU completion tracking (#869 onward).
  Those are library design over the owner facility, and they need only "a type
  that owns handles and cleans up once", which is what sections 4 to 6 give.

The GPU manager in #878 would then be spelled:

```rae
type GpuResources {
  device: Ptr
  queue: Ptr
  textures: List(GpuTexture)
}

func create() ret opt GpuResources { ... }
func drop(this: mod GpuResources) { wgpuQueueRelease(queue: this.queue) wgpuDeviceRelease(device: this.device) }
# `textures` drops after the hook, each element through drop(this: mod GpuTexture).
```

## 10. The destructor's name

The name is the one open choice; the mechanism is the same for any spelling.

| Name | For | Against |
| --- | --- | --- |
| **`drop`** (recommended) | Already the compiler's word for cleanup and the name of the hooks the compiler calls today; zero migration for `List`/maps/ECS tables; Rust readers know it; pairs acceptably with `create` ("create it, drop it") | Slightly less obvious than `delete` to a C/Java reader |
| `destroy` | The C-API pair for create (`SDL_CreateWindow`/`SDL_DestroyWindow`, `wgpu…Create`/`…Destroy`); reads as an action the type performs on its resources | Introduces a second word next to the compiler's "drop"; rename of existing hooks |
| `delete` | Verbal pair of create | Reads as manual heap deallocation (C++ `delete`, JS `delete`) and is a natural method name for "remove an element" (`map.delete(key:)`), which would collide with the reserved shape |
| `free` | Exists today on containers | C's word for raw memory, not resources; no call sites, so nothing is lost by removing it |
| `close`/`release`/`dispose` | Familiar from files/handles | Each already means "explicit, partial, may fail" somewhere in lib; the destructor is implicit and may not fail |

Recommendation: `create` / `drop`. If the maintainer prefers `destroy` for the
create/destroy pairing, the only change to this document is the reserved name.

## 11. Examples end to end

Water, after migration (abridged):

```rae
type WaterSystem {
  bodies: List(WaterBody)
  grid: MeshHandle
  pipeline: Ptr
  uniform: Ptr
  cascades: List(WaterCascade)     # each owns its textures and has a drop
  time: Float
}

func create(body: view WaterBody) ret WaterSystem {
  var waterSystem: WaterSystem = {
    bodies: createList(WaterBody, cap: 4), grid: uploadUnitGrid(segments: body.gridSegments),
    pipeline: webgpuNull(), uniform: webgpuNull(), cascades: createList(WaterCascade, cap: 3), time: 0.0 }
  waterSystem.bodies.add(value: body)
  ret waterSystem
}

func drop(this: mod WaterSystem) {
  if not (this.pipeline is webgpuNull()) { wgpuRenderPipelineRelease(renderPipeline: this.pipeline) }
  if not (this.uniform is webgpuNull()) { wgpuBufferRelease(buffer: this.uniform) }
  # bodies, grid and cascades drop next, cascades element by element.
}
```

An App owning it:

```rae
type App {
  renderer: RendererDeferred
  waterSystem: WaterSystem
}

func main() ret Int {
  var app: App = { renderer: create(), waterSystem: create(body: createLakeToon(surfaceZ: 0.0, extent: 400.0)) }
  loop not Gpu2d.pollClose() { frame(app: app) }
  ret 0                             # app drops: waterSystem first (declared last), then renderer
}
```

Rejected programs, each a separate compile error:

```rae
let second: WaterSystem = app.waterSystem        # ERROR: WaterSystem has a destructor and no copy
func drop(this: own WaterSystem) { }             # ERROR: `drop` is reserved: must be `func drop(this: mod WaterSystem)`
func drop(this: mod WaterSystem) ret Bool { }    # ERROR: same
app.waterSystem.drop()  app.waterSystem.step()   # ERROR: use of dropped value
let anything: Int = create(cap: 4)               # ERROR: no `create` returns Int
```

## 12. Verification for the implementation task

- Hook runs exactly once for: scope exit, early `ret`, `break`/`continue`,
  reassignment, `own` parameter consumed by callee, returned local (not run in
  the callee), `opt T` present/none, field of a struct, element of a `List`,
  element of a component table.
- Explicit `x.drop()` then scope exit: one run. Use after explicit drop: sema
  error.
- Copy of a hook-bearing type by `=`, by `copy` parameter, by struct-literal
  field (`field: x` without `own`): sema errors.
- `create` resolution: by binding type, by parameter type, by field type; error
  without expected type; `T.create` in `ret`; generic `List(String)` binding;
  ambiguity with two visible `create` for one type.
- `Ptr` field allowed only with a hook.
- Existing suites unchanged: 425 (list element drop), the Stage 1 drop tests,
  and the 114/118/119 example gates after the water migration.

Test files use a fake resource (a counter incremented by an extern release
function) so the whole matrix runs without a GPU.
