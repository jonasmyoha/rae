# Compile-time reflection: iterating a struct's fields

**Status:** LANDED for values (#772: non-generic `fields(value)`, `any` wildcard, `fieldName()`) and through a generic world parameter (#773: `func f(W: type, world: mod W) { loop ... in fields(world) }`, expanded per instantiation, W inferred from the argument). First consumer landed (#760): lib/ecs `clearEntityComponents`, UiWorld `destroyEntity`, the 116 registry-gated `worldToJson`/`worldFromJson`, and the 114 despawn seams. `fields(Type)` construction is #774. Supersedes the rejected `clearEntityComponents`
compiler builtin (queue #760) and the vague "derive/reflection" bullet in
`docs/ecs-language-wishlist.md`. See that file for the rules this design obeys.

## The problem it solves

Every ECS world hand-lists its component tables in every whole-world operation:
`destroyEntity` is one `componentRemove` per table, `worldToJson` is one
`addComponentTable` per table, `createX` constructs each table by hand. Add a
component, edit three places. Rae has no field reflection and no function values, so
nothing can hold a world's heterogeneous `ComponentTable(T)` values as data — the
registry (#730) stores names + flags, not the tables. So the listing cannot be
factored out in Rae today.

Two ways to fix that are OFF the table, per `AGENTS.md`:

- a one-use compiler/C-backend builtin (`clearEntityComponents` synthesised in
  sema/c_backend) — a bespoke feature for a single job, the forbidden hand-added
  C-surface;
- any `@`-sigil attribute (`@derive`, `@world`) — a second keyword vocabulary.

What is wanted is a small, GENERAL capability, expressed with real keywords and plain
functions, that lets clear / serialize / construct / despawn each be written **once,
generically, in Rae** — and compile to exactly the code you would have written by hand.

## The design in one sentence

Iterating a struct's fields is the **existing collection loop** with a
**compile-time sequence** (`fields(x)`) after `in`, unrolled by the compiler; the
binding's required explicit type is the **filter**, `any` is the type wildcard, and
slot metadata is read through **compile-time plain functions** like `fieldName(x)`.

## Building block 1 — the collection loop Rae already has

Real code today:

```rae
loop let sp: view Sphere in world { ... }          # view alias per element
loop var mutableValue: mod Int in values { ... }   # mod alias per element
loop let value: Int in data { ... }                # copy per element
```

`loop let|var <name>: <mode> <Type> in <collection> { }`. The binding **requires an
explicit type**; `let` for view/mod aliases, `var` for copies. The parser already
parses this (`loop_stmt.is_range`). Nothing below changes the grammar of this form.

## Building block 2 — a struct's fields are a compile-time sequence

`fields(x)` yields the field set of a struct **value** `x`; `fields(T)` yields the
field set of a struct **type** `T` (no value needed — this is what construction
uses). Both are compile-time sequences, valid only in iterable position. Passing a
type to a function is already a Rae idiom (`createList(String, cap: 4)`,
`createComponentTable(Rect)`), so `fields(UiWorld)` is not a new shape.

```rae
loop let f: view any in fields(world) { ... }    # every field of the value
loop let f: any in fields(UiWorld) { ... }       # every field of the type
```

Why `fields(world)` and not bare `in world`: iterating a struct value directly is
semantically odd (a struct is not a sequence), leaves the compile-time-ness implicit
(you must know `world` is a struct, not a List, to know the loop unrolls), and does
not extend to iterating a *type*. `fields(...)` names the thing that actually is a
sequence, is greppable as reflection, and works for both values and types.

Why a **call** and not a property (`world.fields`): a property is a phantom member.
`fields` is a very plausible real field name (a form has fields), so `world.fields`
collides; a property also looks like runtime data. A call is unambiguous, lives in
the function namespace, and signals "resolved by the compiler". Via UFCS it is still
writable as `world.fields()`.

## Building block 3 — the binding type is the filter; `any` is the wildcard

The explicit binding type the collection loop already demands doubles as the field
filter: only fields whose type matches are bound; the rest are skipped. No `where`,
no `if`, no `_`.

```rae
# every ComponentTable field, any element type
loop let table: mod ComponentTable(any) in fields(world) { ... }

# only the ComponentTable(Transform3D) fields
loop let table: mod ComponentTable(Transform3D) in fields(world) { ... }

# every field regardless of type — the body may then only do what is valid for
# every field's type (checked once per field)
loop let field: view any in fields(world) { ... }
```

`any` is the **one new keyword**: the compile-time type wildcard, legal only inside
a type pattern. It is deliberately NOT the existing type `Any`, and the two cannot be
merged: `Any` is the *runtime* dynamic-value box (`RaeAny` — `log(value: Any)`,
`Buffer(Any)`, `List2` elements), so `ComponentTable(Any)` is *already* a legal
concrete type today meaning "a table of dynamic boxes". Reusing it in a pattern would
make `loop let t: ComponentTable(Any) in fields(world)` ambiguous between "only fields
of that exact concrete type" and "tables of any element type". Hence: `Any` = runtime
box (a value exists), `any` = compile-time wildcard (no value, only a pattern).
Lowercase `any` is reserved as a keyword outright; no project code uses it as an
identifier. (Decided 2026-09-02.)

For dispatch over several field kinds, `match` keeps its existing shape:

```rae
loop let field: view any in fields(world) {
  match field {
    case ComponentTable(any) { ... }
    case EntityAllocator     { ... }
    case HierarchyOrder      { ... }
  }
}
```

## Building block 4 — slot metadata is a compile-time plain function

The name of the slot a binding came from is a compile-time string, read with a
plain camelCase function the compiler folds to a literal:

```rae
fieldName(table)      # -> "rects"   (also table.fieldName() via UFCS)
```

Not `table.fieldName` (no parens): the binding IS the value (`world.rects`), so a
paren-less property pretends the `ComponentTable` knows its own slot name (a category
error), reads as runtime data, and collides with any real field called `fieldName`.

## The rule that ties it together

**All reflection data is a compile-time plain function, always called with parens,
reachable via UFCS. There are no phantom members, ever.**

- `fields(value)` / `value.fields()` — field set of a value
- `fields(Type)` — field set of a type
- `fieldName(binding)` / `binding.fieldName()` — the slot name, folded to a literal

Two small, general primitives — which is exactly what separates this from the rejected
#760 builtin (one function, one job, baked into the backend).

## Worked examples

```rae
# Clear every component of an entity — replaces the hand-listed destroyEntity loop.
func clearEntityComponents(W: type, world: mod W, entity: view EntityId) {
  loop let table: mod ComponentTable(any) in fields(world) {
    componentRemove(this: table, entity: entity)
  }
}

# Serialize — the registry's flags still gate WHICH tables serialize; the loop
# only removes the hand-listing. No member annotations anywhere.
func worldToJson(W: type, world: view W, reg: view ComponentRegistry) ret String {
  var w: WorldJson = createWorldJson()
  loop let table: view ComponentTable(any) in fields(world) {
    addComponentTable(writer: w, table: table, name: fieldName(table), registry: reg)
  }
  ret w.finish()
}

# Construct — iterate the TYPE's fields; no value exists yet.
loop let slot: ComponentTable(any) in fields(UiWorld) {
  ...   # emit createComponentTable(T) for each slot
}
```

What `clearEntityComponents(world: uiWorld, ...)` unrolls to for `UiWorld` — exactly
the hand-written loop, nothing more:

```rae
componentRemove(this: world.rects,   entity: entity)
componentRemove(this: world.sizes,   entity: entity)
componentRemove(this: world.layouts, entity: entity)
# ... one per ComponentTable field; allocator / hierarchyOrder skipped
```

## Semantics

- **Compile-time, unrolled.** The body is emitted once per matching field with the
  binding substituted by `x.<field>` at that field's type. Zero runtime cost; the
  output is a pure function of the type; a tool can expand it exactly as the
  compiler does. Deterministic and analyzable — the constraint that rules out
  Jai-style `#run`.
- **Type-checked per field.** The body must be valid for every field the filter
  admits. `loop let f: view any` admits everything, so the body may only use what
  every field type supports; an error points at the offending field.
- **Binding modes are the existing rules.** `let … mod` is a write-through alias to
  `x.<field>` (what `componentRemove` needs); `let … view` is read-only; `var` would
  be a *copy* of a `ComponentTable`, which the current "reference bindings are
  aliases; use `let`" rule already rejects.

## How the compiler recognises it

- **Parser:** unchanged. `loop let x: T in EXPR` already parses.
- **Sema:** where it checks "the iterable is a `List(T)`", add "the iterable is
  `fields(...)`" → a field loop. Resolve the struct (value or type), apply the
  binding-type filter, and type-check the body once per matching field with the
  binding bound to `x.<field>`. `any` unifies with any type in a pattern.
- **Codegen:** unroll — emit the body per matching field; fold `fieldName(x)` to the
  string literal.

`fields`/`fieldName`/`any` are compiler-recognised (like `sizeof`, or the type
argument of `createList`), but they are *general* — they express a capability, not a
single helper.

## What this deliberately is NOT

- **Not compile-time code execution** (Jai `#run`/`#insert`). Only structural
  unrolling over a type's fields — no arbitrary metaprogramming, no macro system.
- **Not runtime reflection** (Odin `type_info_of`). No runtime type info, no
  runtime cost, no runtime field walk. Runtime reflection for tooling/editors is a
  separate, later question, orthogonal to this.
- **Not attributes.** No `@`-sigils, no member annotations (Jai `@note` style).
  "Which fields?" is the type filter; "which of them serialize?" is the existing
  registry's flags. Nothing new is stapled onto declarations.

## Comparison

- **Jai:** runtime `type_info(T).members` plus `#run`/`#insert` compile-time
  execution that can generate per-field code; member `@notes` for annotations.
  Powerful; too open for Rae's determinism/analyzability goals.
- **Odin:** runtime `type_info_of` / `core:reflect` field iteration plus compile-time
  intrinsics such as `type_is_specialization_of`; Go-style backtick tags; no `#run`.
  Field *iteration* is a runtime loop.
- **Rae (this design):** the compile-time result Jai gets, via restricted structural
  unrolling instead of arbitrary execution; the type-query precision Odin has, via
  the binding type; no tags, no notes, no sigils.

## Open questions

- Does a field binding need `match` over its type, or is the binding-type filter
  enough in practice? (Start with the filter only; add `match` if a real need appears.)
- Construction via `fields(Type)`: how the loop body assembles the struct literal
  (per-slot initializer emission) needs its own small design.
- Nested structs: does `fields()` recurse, or only one level? (One level; recurse
  explicitly with a nested loop.)
- Inferring the world type `W` from the value argument, so callers write
  `clearEntityComponents(world: uiWorld, entity: e)` without an explicit `W:`.
