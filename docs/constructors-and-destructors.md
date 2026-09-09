# Constructors, destructors and copies in Rae: `create`, `drop`, `copy`

Status: **design proposal, 2026-09-09, for maintainer approval.** Nothing here is
implemented. This document covers constructors and destructors only. Native
pointers, `unsafe`, GPU resource identity and the rest of
`ptr-and-gpu-resource-design.md` (#878) are out of scope; section 8 says only
which #878 spellings this replaces.

## 1. The idea

A **constructor** is an ordinary free function named `create` that returns the
type. A **destructor** is an ordinary free function named `drop` whose first
parameter is `this: mod T`. A **copy** is an ordinary free function named
`copy` that takes `this: view T` and returns a new `T`. All three are written
the way every Rae method is written today (`func size(this: view Buffer) ret
Int`). There are no new keywords, no type properties, no attributes and no
registration. The **name and the signature** are the whole contract, exactly
as a first parameter named `this` is the whole contract for member-call sugar.

The three are one design and can be three implementation tasks (#880 `create`,
#881 `drop`, #882 `copy`); section 5 says why `drop` without `copy` is already
a consistent state.

```rae
type Counter {
  handle: Int          # a slot number in some external table
  count: Int
}

func create(start: view Int) ret opt Counter {
  let handle: Int = counterTableAcquire()      # some resource you have to give back
  if handle < 0 { ret none }
  ret Counter { handle: handle, count: start }
}

func drop(this: mod Counter) {
  counterTableRelease(slot: this.handle)
}

func copy(this: view Counter) ret Counter {
  let handle: Int = counterTableAcquire()      # a second slot, not the same one twice
  ret Counter { handle: handle, count: this.count }
}

func bump(this: mod Counter) {
  this.count = this.count + 1
}

func main() {
  if let counter: Counter = create(start: 3) {
    let twin: Counter = counter      # copy(this: counter): its own slot
    counter.bump()
  }                        # drop runs for twin, then for counter, each exactly once
}
```

That is the whole feature from the user's side.

## 2. Why by name

Rae already has two conventions that work purely by name and signature:

- **Member-call sugar** (`spec/rae.md` §5.3): any function whose first
  parameter is of type `T` can be called as `value.f()`. No `method` keyword, no
  `impl` block. The function's shape makes it a method.
- **The container destructor that exists today.** `compiler/src/c_stmt.c`
  (`find_drop_overload_for`) already looks for a user function literally named
  `drop` whose first parameter is the container type, and calls it during
  cleanup. `lib/core/List.rae`, `lib/collections/*.rae` and `lib/ui/ecs.rae`
  all define `func drop(T: type, this: mod X(T))`, and the compiler already runs
  them. The only limitation is that the lookup is restricted to generic
  containers.

So the destructor half of this design is not new machinery: it lifts the
restriction that a `drop` must be on a generic container. The constructor half
adds one resolution rule for the name `create`.

The #878 spelling, `type NativeBuffer: opaque noncopyable drop`, goes the other
way: three type properties, and the rule that "existing functions named `drop`
do not silently become hooks". This design says the opposite: **a function
named `drop` with the destructor signature IS the destructor**, in the same way
that `func size(this: view Water)` IS a method without any annotation on `Water`.

## 3. `create`: the constructor

### 3.1 Definition

A constructor of `T` is any function named `create` whose declared return type
is `T` or `opt T`. Its parameters are ordinary (every one with an explicit
mode). Its body builds the value with the normal struct literal; nothing about
construction changes inside the function.

```rae
func create(cap: view Int) ret List(T)                   # generic: T comes from the caller's expected type
func create(surfaceZ: copy Float, extent: copy Float) ret WaterBody
func create(body: view WaterBody) ret WaterSystem
func create(start: view Int) ret opt Counter             # fallible: none on failure
```

Returning `opt T` is the fallible constructor. There is no other failure
channel: a constructor that cannot fail returns `T`, one that can returns
`opt T`, and the caller uses `if let` as for any optional.

### 3.2 Resolution: `create` follows the struct-literal rule

Rae has no return-type overloading and no type inference, so a bare call
`create(...)` must know which type it produces. The rule is the one the untyped
struct literal already has: **`create(...)` is resolved by the expected type of
the position it appears in, and is legal exactly where an untyped literal
`{ ... }` is legal.**

```rae
let names: List(String) = create(cap: 4)          # expected type List(String); T = String
var water: WaterSystem = create(body: lake)       # expected type WaterSystem
let bed: Material3d = { baseColor: sand, ... }    # the existing literal rule, same positions
```

Where an untyped literal must be typed, `create` is qualified with the type:

```rae
ret WaterBody.create(surfaceZ: 0.0, extent: 400.0)   # ret has no left-hand type
log(create(start: 3))                                # ERROR: no expected type; write Counter.create(...)
```

`T.create(...)` is the type-qualified call: the type before the dot selects
the constructor whose return type is `T` or `opt T`. It mirrors member-call
sugar (an instance before the dot selects by first-parameter type; a type
before the dot selects by return type). Generic types take their arguments from
the expected type or from the qualifier: `List(String).create(cap: 4)`.

Module qualification keeps working (`Water.create(...)` when the module is
named `Water`) but is not the mechanism and is not required.

### 3.3 Rules

1. **At most one `create` per type in scope.** Two visible `create` functions
   returning the same `T` is an ambiguity error at the call. Named factories
   (`createLakeToon`, `createRiverToon`) stay ordinary functions with ordinary
   names; they are not constructors in the language sense.
2. **`create` is not required.** A type without one is built with the struct
   literal, as now, and a type with one can still be built with the literal.
   This design adds no field privacy.
3. **`create` is not special inside.** It is a normal function body: it may call
   other constructors, return `none`, and its locals drop on an early `ret`
   like any function's. A constructor that creates two owned things and fails
   on the second therefore already cleans up the first, by the existing rules.
4. **Expected-type resolution is defined for the name `create` only**, as the
   literal rule is defined for `{ }` only. It is not a general feature.

### 3.4 What `create` buys

- One name for "make me one of these" across every type: `create(cap: 4)` for
  a list, `create(body: lake)` for a water system. Today it is `createList`,
  `createWaterSystem`, `createComponentTable`, one spelling per type.
- The type is written once, on the left, which is the rule Rae already enforces
  for literals (`let p: Point = { x: 1 }`, never `Point { ... }` twice).
- Tooling and agents find every type's constructor by name and return type.
- Constructor and destructor become a visible pair in one file. `createList`,
  `free`, `shutdownAll`, `waterShutdown`, `gbReleaseTargets` collapse into one
  vocabulary: `create` gives you the value, `drop` takes it back.

## 4. `drop`: the destructor

### 4.1 Definition

A destructor of `T` is a function of exactly this shape, declared in the module
that declares `T` (the same file, or a sibling file merged into that module):

```rae
func drop(this: mod T)
```

For a generic type the shape is the one the stdlib already uses:

```rae
func drop(T: type, this: mod List(T))
```

No return type. `this` is `mod`, never `own`: the destructor cleans the value
up in place and the compiler owns the storage. Any other function named `drop`
whose first parameter is a `T` is a compile error (`drop` is reserved for the
destructor of `T`; it must be `func drop(this: mod T)`). A `drop` for a type
from another module is an error too. Name plus shape is the entire opt-in.

### 4.2 When it runs

The compiler already emits cleanup for every value that owns heap
(`docs/drop-semantics-and-defer.md`, Stage 1): at scope exit, before every
`ret`, before reassignment, on `break`/`continue`, in reverse binding order,
with `ret ident` moving instead of dropping. This design changes only which
types count and what dropping one does:

- **Which types.** A type needs cleanup if it has a destructor, or if any field
  (transitively, through generics and `opt`) needs cleanup. Today the base case
  is hard-coded to `String`, `List`, `StringMap`, `IntMap`; it becomes "has a
  destructor or contains a type that has one", with those stdlib types simply
  being types that have destructors.
- **What dropping does.** Run `T`'s destructor if it has one, then drop each
  field that needs cleanup, in **reverse declaration order**. The destructor
  runs first so it sees every field live (a renderer can flush before its
  buffers go). The destructor must not drop its own fields; the compiler does
  that next. Elements of a `List(T)` drop before the list's storage, which is
  the per-element loop the backend already injects for stdlib containers.
- **Once.** Each initialized value that has not been moved out drops exactly
  once. `own` parameters, `own` fields, `if let` payloads and returned locals
  follow the existing move rules; moved-from storage is skipped.

Nothing runs on process abort or an unhandled panic; that is unchanged and not
promised. `defer` stays a separate statement for caller-side cleanup and is not
a substitute (same conclusion as `drop-semantics-and-defer.md`).

### 4.3 Explicit early destruction

`value.drop()` is a normal call of the destructor through member-call sugar.
The compiler treats it as **consuming the value**: after `water.drop()`,
`water` is moved-from, any later use is a compile error, and scope exit skips
it.

```rae
var water: WaterSystem = create(body: lake)
water.drop()                 # releases now; `water` is consumed
water.step()                 # ERROR: use of dropped value `water`
```

A type may still offer `close` or `shutdown` for a partial, resumable release
(stop accepting work, keep the value alive). That is an ordinary method with no
language meaning.

### 4.4 What a destructor may do

- Call release functions, other methods on `this`, and log.
- Read and write its own fields; they are all live, and the ones that need
  cleanup drop afterwards.
- Not: return a value, take `this` by `own`, move `this` or a field out.
- Not fail. A release that can report an error belongs in an explicit `close`
  method that returns the error; the destructor is best effort.

## 5. `copy`: the copy constructor

### 5.1 The problem it solves

A plain `=` on a value that owns something makes a second owner, and at scope
exit both are dropped. For `List` and `String` that is fine because `=` is a
deep copy the compiler knows how to make: two buffers, two drops. For a type
with a user destructor the compiler cannot make a second resource; it only
knows how to copy the bits, which would duplicate a handle and release it
twice. So a type with a destructor needs to say how it is copied, or say that
it cannot be.

### 5.2 Definition

A copy of `T` is a function of exactly this shape, in the module that declares
`T`:

```rae
func copy(this: view T) ret T
```

It receives the original by `view` and returns a new, independent value. Any
other function named `copy` whose first parameter is a `T` is a compile error
(`copy` is reserved for the copy of `T`). Explicit calls, `let twin: Counter =
counter.copy()`, are ordinary calls.

### 5.3 When it runs

`copy` is what `=` means for that type. The compiler calls it everywhere it
performs a value copy today:

```rae
let twin: Counter = counter               # copy(this: counter)
useCounter(value: counter)                # `copy` parameter: copy(this: counter)
let pair: Pair = { left: counter, ... }   # literal field without own: copy(this: counter)
if let element: Counter = list.copyAt(index: 0) { ... }   # element copy: copy(this: element)
```

A struct that contains a `Counter` field and has no `copy` of its own is
copied structurally, as today, with the field copied through `copy`. A struct
that defines its own `copy` replaces the structural copy entirely and is
responsible for every field. A type without a destructor may also define
`copy`; it simply overrides the structural deep copy.

### 5.4 The rule for types with a destructor and no `copy`

**A type that has a destructor (or contains one) and has no `copy` cannot be
copied.** `=`, `copy` parameters, literal fields without `own` and `copyAt` on
a container of it are compile errors. The value moves with `own` and is
borrowed with `view`/`mod`.

```rae
let alias: Session = session              # ERROR: Session has a destructor and no copy
useSession(initial: own session)          # ok: transfer
inspect(session: session)                 # ok: borrow
```

This is the point of shipping `drop` before `copy`: with the error in place a
destructor is safe on its own, and adding `copy` later only removes errors,
never changes the meaning of code that compiled.

### 5.5 What `copy` may do

- Acquire a new resource for the result and read anything from `this`.
- Return `T` only. A copy that can fail is not a copy; provide a named function
  returning `opt T` and let callers move or borrow instead.
- Not modify `this` (it is `view`), and not return `this` itself.

Containers follow the element: `List(Counter)` copies element by element
through `copy`, and `List(Session)` is non-copyable.

## 6. What changes in the compiler

Each row maps onto machinery that already exists.

| Area | Today | Change |
| --- | --- | --- |
| Destructor lookup (`c_stmt.c` `find_drop_overload_for`, `c_backend.c` struct-drop synthesis) | Generic containers only, matched by first-parameter base name | Any type; exact shape `func drop(this: mod T)`; same-module check; reserved-name diagnostic for other shapes |
| Which types need cleanup (`ownership.c` `is_drop_target_type`, `type_needs_cascade_drop`) | Hard-coded `String`/`List`/`StringMap`/`IntMap` | "has a destructor or contains one" |
| Dropping a struct | Fields only | Destructor first, then fields in reverse declaration order |
| Explicit `x.drop()` | Ordinary call, value dropped again at scope exit | Marks the local moved (`local_moved`); later use is a sema error |
| Copy (sema + `c_backend.c` `rae_deep_copy_<T>`) | Structural deep copy for heap types | Call the user `copy` where one exists; structural copy recurses into it for fields; reject copies of destructor-bearing types without one |
| `create` resolution (sema) | None | Resolve by expected type in literal positions; `T.create(...)`; ambiguity diagnostic |
| Lists of such types | Element loop exists for stdlib | Same loop calls the user destructor |

The VM target is not touched (deprecated).

## 7. Migration in `lib` and examples

- `createList(T, cap)`, `createStringMap`, `createComponentTable(T)` gain a
  `create` twin; the old names stay one release as aliases, then go.
- `free()` on `List`/`StringMap`/`IntMap` is deleted: it has no call sites, and
  `drop` is the destructor. `docs/STDLIB_GUIDE.md`'s "must provide `free()`"
  becomes "define `drop` if you hold anything the compiler cannot see".
- `waterShutdown` becomes `drop(this: mod WaterSystem)`; `Gbuffer.shutdownAll`,
  `shadowShutdown`, `skinShutdown`, `deferredShutdown` become destructors on
  the owning renderer/App resources.
- Examples stop calling any shutdown function at the end of `main`; the App
  local's drop does it.

## 8. Relation to #878

If approved, this replaces these #878 spellings:

| #878 | Here |
| --- | --- |
| `drop` type property + "exactly one defining-module hook" | The destructor by name and shape, section 4.1 |
| `noncopyable` type property | Derived: a destructor without a `copy` means not copyable, section 5.4; `copy` makes it copyable again |
| "Direct calls to the hook are rejected; `close` must be idempotent" | `x.drop()` consumes the value, section 4.3 |
| "Reject owners in containers initially" | Allowed; containers inherit non-copyability, section 5 |
| `createNativeBuffer` named factory | `create`, section 3 |

Everything else in #878 (`opaque`, `unsafe`, extern obligations, resource IDs)
is a separate decision that this document neither needs nor contradicts.

## 9. The destructor's name

The mechanism is the same for any spelling; the name is the open choice.

| Name | For | Against |
| --- | --- | --- |
| **`drop`** (recommended) | Already the compiler's word for cleanup and the name of the destructors the compiler calls today, so nothing migrates; Rust readers know it; "create it, drop it" | Less obvious than `delete` to a C or Java reader |
| `destroy` | The C-API pair for create (`SDL_CreateWindow`/`SDL_DestroyWindow`); reads as an action the type performs on its resources | A second word next to the compiler's "drop"; renames existing container destructors |
| `delete` | Verbal pair of create | Reads as manual heap deallocation (C++/JS `delete`) and is a natural method name for "remove an element" (`map.delete(key:)`), which would collide with the reserved shape |
| `free` | Exists today on containers | C's word for raw memory; no call sites, so nothing is lost by removing it |
| `close`/`release`/`dispose` | Familiar from files and handles | Each already means "explicit, partial, may fail" somewhere in lib; the destructor is implicit and may not fail |

Recommendation: `create` / `drop` / `copy`. If the maintainer prefers `destroy`,
the only change to this document is the reserved name.

## 10. Examples end to end

An App owning a water system:

```rae
type App {
  renderer: RendererDeferred
  waterSystem: WaterSystem
}

func drop(this: mod WaterSystem) {
  releaseSurfacePipeline(waterSystem: this)
  # bodies, grid and cascades drop next, cascades element by element.
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

## 11. Verification for the implementation task

- The destructor runs exactly once for: scope exit, early `ret`,
  `break`/`continue`, reassignment, an `own` parameter consumed by the callee,
  a returned local (not run in the callee), `opt T` present and none, a field
  of a struct, an element of a `List`, an element of a component table.
- Explicit `x.drop()` then scope exit: one run. Use after explicit drop: sema
  error.
- Copy of a type with a destructor and no `copy`, by `=`, by `copy` parameter,
  by a struct-literal field without `own`, by `copyAt`: sema errors.
- With a `copy` defined: each of those calls it once, the result is independent
  (drop both, the fake resource counter shows two releases), a struct containing
  the type copies structurally through it, and a `copy` of the wrong shape is a
  declaration error.
- `create` resolution: by binding type, parameter type, field type; error
  without an expected type; `T.create` in `ret`; generic `List(String)`
  binding; ambiguity with two visible `create` for one type.
- Existing suites unchanged: 425 (list element drop), the Stage 1 drop tests.

Test programs use a fake resource, a counter bumped by the destructor, so the
whole matrix runs without any native library.
