# Compile-time reflection: iterating a struct's fields

**Status:** LANDED for values (#772: non-generic `fields(value)`, `any` wildcard, `fieldName()`) and through a generic world parameter (#773: `func f(W: type, world: mod W) { loop ... in fields(world) }`, expanded per instantiation, W inferred from the argument). First consumer landed (#760): lib/ecs `clearEntityComponents`, UiWorld `destroyEntity`, the 116 registry-gated `worldToJson`/`worldFromJson`, and the 114 despawn seams. `fields(Type)` construction was assessed and **declined** (#774 — see "Construction via `fields(Type)` — decided NOT to build" below). Supersedes the rejected `clearEntityComponents`
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
`Buffer(Any)`, `List(Any)` elements), so `ComponentTable(Any)` is *already* a legal
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
typeName(table)       # -> "Rect"    (also table.typeName()  via UFCS)   #809
```

`fieldName` is the SLOT name; `typeName` is the COMPONENT type name — the `T` in
`ComponentTable(T)`, or a non-generic field's own type (`Int` for `count: Int`).
They are different vocabularies for different consumers: the machine snapshot and
the registry key by the component name (`Position`), the same name a human writes
in a `.raescene`, so `typeName` lets one registry vocabulary serve both instead of
forcing the registry to be re-keyed by slot name (`positions`).

Not `table.fieldName` (no parens): the binding IS the value (`world.rects`), so a
paren-less property pretends the `ComponentTable` knows its own slot name (a category
error), reads as runtime data, and collides with any real field called `fieldName`.

## The rule that ties it together

**All reflection data is a compile-time plain function, always called with parens,
reachable via UFCS. There are no phantom members, ever.**

- `fields(value)` / `value.fields()` — field set of a value
- `fields(Type)` — field set of a type
- `fieldName(binding)` / `binding.fieldName()` — the slot name, folded to a literal
- `typeName(binding)` / `binding.typeName()` — the component/element type name,
  folded to a literal (#809)

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

# Construct — iterate the TYPE's fields; no value exists yet. ILLUSTRATIVE ONLY:
# this half was assessed and declined (#774) — see the decision section below.
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

## Mutable iteration + `fieldSet` + default value (#959)

The read side above (`fieldName`, `typeName`, a `mod` binding for *method
calls*) was enough for clear/serialize. A generic *decoder* — the registry
synthesis that replaces one hand-written arm per component (#961) — needs
two more things: a way to WRITE a field wholesale through the binding, and a
value to start from. Both are additions to iteration over an EXISTING value;
neither is construction by reflection (the #774 decision stands).

**`fieldSet(f, value: v)` — wholesale field write.** Inside a `mod` field
loop, the statement `fieldSet(f, value: v)` (or its UFCS spelling
`f.set(value: v)`) assigns `v` to the field the binding currently aliases.
It is a compile-time plain function like `fieldName`: the unroller rewrites
the statement into the ordinary assignment `x.<field> = v`, so everything
that holds for an assignment holds here — the value's type must equal the
field's type (checked per unrolled field, with the diagnostic on the
`fieldSet` line: `type mismatch: expected Int, got String`), a String or List
moves/copies under the normal ownership rules, and the old value is dropped.
There is no runtime `set` method and no phantom member; `fieldSet` outside a
field loop is simply an unknown function.

```rae
func fill(comp: mod Comp) {
  loop let f: mod Int in fields(comp) {
    fieldSet(f, value: 7)
  }
  loop let f: mod String in fields(comp) {
    f.set(value: "named")
  }
}
```

Wholesale write is the only new capability: `f.set(value:)` assigns the whole
field; per-field *method* calls through the `mod` binding were already there.

**`T.default()` — the default value.** A compiler-generated static method
with the same shape as `T.fromJson(json:)`: it yields the value whose every
field is its DECLARED default (`value: Bool = true`, `tint: RgbaColor =
RgbaColor { ... }`, `angle: Float = 90.0` — the field-default syntax the
parser always accepted, given meaning by #961) or, absent one, zero / `0.0` /
`false` / `""` / an empty List / the first enum case / `none` for `opt`;
a nested user struct without a declared default is its own `default()`,
recursively. It is ONE opaque value primitive (the C backend emits a generated
`rae_default_<T>()` per non-generic user struct, `(C){0}` for everything
else), not field-by-field construction — you cannot ask what it did per
field, only start from it. `T` may be a
generic parameter (`func zeroOf(T: type) ret T { ret T.default() }`) or a
builtin (`Int.default()` is `0`, `String.default()` is `""`). Like
`fromJson`, constructing a value that contains raw `Ptr` storage this way
needs an enclosing `unsafe { }` (#896).

A generic decoder is therefore:

```rae
var comp: T = T.default()
loop let f: mod any in fields(comp) {
  # decide per field what to write; fieldName(f) / typeName(f) fold as before
  f.set(value: decodeField(f, doc: doc))     # #960 supplies decodeField
}
componentSet(table, entity, comp)
```

**Optional fields.** A pattern's optionality is part of the match: an
`opt T` field is bound only by an `opt T` pattern (the alias then carries the
`opt`); a plain `T` or `any` pattern skips it. Before #959 the filter ignored
`opt` and a plain pattern would alias an un-unwrapped optional, which the
alias binding then rejected.

Fixtures: `843_field_set_default` (both spellings, the generic `W` path, the
`T.default()` → `toJson` → `fromJson` → `toJson` round-trip, builtin and
nested-struct defaults), `844_field_set_type_mismatch` (the per-field check).

## Enum lookup, and generic bodies that branch on the type (#960)

Two more compile-time plain functions, and one rule about `if`, complete the
kit a generic *decoder* needs — the piece that lets #961 replace one
hand-written registry arm per component with a field loop.

**`enumFromName(E, name: s)` → `opt E`.** The member of enum `E` whose case
name is `s`, or none. One lookup table per enum, generated by the compiler
(the mirror of the `toString` every enum already has); exact and
case-sensitive — the authored PascalCase vocabulary (`"Horizontal"`) is
lowered by the caller (`authoredEnumName` in `lib/ui/RegistryGeneric.rae`),
not guessed by the lookup. `E` may be a generic type parameter, in which case
the instantiation decides: for an enum `T` it is the real lookup, for any
other `T` it is the constant none. That second half is deliberate — a
generic decoder can try the enum path last without first asking "is `T` an
enum?", and a non-enum instantiation costs a constant.

```rae
if let kind: LayoutType = enumFromName(LayoutType, name: "horizontal") {
  ret kind
}
ret LayoutType.none
```

**`typeName(x)` on a generic-typed parameter.** Inside a field loop
`typeName(f)` folded to the field's type name already (#809). It now also
answers for a *parameter* whose type is (or contains) a generic parameter:
`func decode(T: type, fallback: copy T, ...) { if typeName(fallback) is
"Int" { ... } }`. In the template it is a String; in each instantiation it
is that instantiation's concrete type name, folded to a literal. For a
parameter the name is the type's OWN base name — `List` for a `List(String)`
argument — where a field-loop binding over `ComponentTable(Position)` folds
to the element `Position` (#809): the parameter IS the thing, the table
holds the thing. (So a `List(String)` field never takes a decoder's `"String"`
arm.)

**Constant `if` is decided at fold time.** Wherever the compiler folds these
queries — a field-loop body per field, a generic body per instantiation — an
`if` whose condition has become `<literal> is <literal>` (or `is not`) is
decided right there, and the untaken arm is REMOVED before anything
type-checks it. That is the whole trick behind one body serving every type:

```rae
func decodeField(T: type, fallback: copy T, doc: view JsonDoc, val: view JsonValue,
                 key: view String, theme: view UiTheme) ret T {
  if typeName(fallback) is "Int" {
    ret optInt(doc: doc, container: val, key: key, fallback: fallback)   # only in the Int body
  }
  if typeName(fallback) is "String" {
    ret optString(doc: doc, container: val, key: key, fallback: fallback) # only in the String body
  }
  let text: String = optString(doc: doc, container: val, key: key, fallback: "")
  if let member: T = enumFromName(T, name: authoredEnumName(text: text)) {
    ret member                                                             # enum T; none otherwise
  }
  ret fallback
}
```

The Int instantiation never contains the String arm, so `ret optString(...)`
never has to type-check against `Int`. This is still structural unrolling —
no execution, no macro — just applied to a condition that has become a
constant. It is limited to string-literal `is` / `is not` conditions, and
`and` / `or` of those, on purpose: the queries produce Strings, so that is
exactly the shape a type test (or an exclude list of type names) has; a
general constant folder is not on the table.

A generic body may also run a field loop over a typed LOCAL, not only a
parameter — `var comp: T = T.default()` then `loop ... in fields(comp)` —
which is what `decodeComponent` needs (#961).

Together with `T.default()` and `fieldSet` (#959) a component deserializer
is a field loop (see `lib/ui/RegistryGeneric.rae` and fixture
`846_decode_field`):

```rae
var comp: T = T.default()
loop let f: mod any in fields(comp) {
  f.set(value: decodeField(fallback: f, doc: doc, val: val, key: fieldName(f), theme: theme))
}
```

Fixtures: `845_enum_from_name` (member / unknown / empty / case-sensitive,
generic `T`, non-enum `T`), `846_decode_field` (Int, Float plain and space
token, `radius` token, Bool, String, two enums from authored PascalCase, a
missing key keeping the default, an unknown enum spelling keeping the default).

## The scene registry as one reflected loop (#961)

The consumer the whole kit was built for: `lib/ui/Registry.applyComponentByName`
is one `loop let table: mod ComponentTable(any) in fields(world)`. Per table,
a constant `if` of `typeName(table) is not "<runtime table>"` tests excludes
the derived/runtime tables at fold time (they instantiate no decoder), the
authored key is compared with `typeName(table)` (one alias, `Overflow` ->
`OverflowPolicy`), and a match calls `decodeComponent(table: table, ...)` —
`T` inferred from the table — which is `T.default()` + a `mod any` field loop
of `decodeField` + `componentSet`. The 35 hand-written `deser<Component>`
functions and the 39-arm name ladder are gone; what remains explicit is
`ContainerStyle` (no table of its own) and a fixup block for the authored
shapes that are not field-structural (Rect's `{x,y,w,h}`, Layout's `type`
key, flattened insets, palette-slot mirrors, presence flags, two arrays). A
component's authored grammar is now its Rae fields plus their declared
defaults; a component that was "registered but unsupported" before decodes
structurally, and an authored key naming a runtime table is the new hard
error (fixture 840).

## Construction via `fields(Type)` — decided NOT to build (#774)

The clear/serialize halves landed because forgetting a table there is a **silent**
bug: `destroyEntity` leaves stale component data on a recycled index, and a missing
table drops silently from the JSON. Reflection over `fields(value)` removed the
hand-listing that caused those. Construction is the opposite case, so #774 is closed
as not-needed:

- **The failure mode it would prevent is already a compile error.** Rae builds a
  struct from a literal with *every* field present; omit one and it does not compile.
  `createUiWorld` / `createWorld3d` / `createGameWorld` cannot silently drop a table
  the way a hand-listed clear/serialize loop can. Reflection buys no safety here —
  only brevity.
- **It is the cheapest boilerplate and written once per world.** One line per field,
  in a single constructor per world type, touched only when the world gains a field
  (which the compiler then forces you to initialise).
- **It is the most expensive to implement, for the least gain.** Unlike
  clear/serialize — which ITERATE an existing value and call one side-effecting op per
  field — construction has no value to iterate and no incremental-assembly form in the
  language: a struct is built all-at-once from a literal. A generic
  `createWorld(W: type) ret W` would need (a) type-level `fields(Type)` over a type
  with no value, (b) a NEW struct-assembly construct to accumulate per-field
  initialisers into a literal (the unsolved "how the loop body assembles the struct"
  question), and (c) per-field-kind dispatch, because the worlds are NOT uniform:
  alongside `ComponentTable(T)` fields they carry an `EntityAllocator`, a `StringMap`
  (`UiWorld.nodeIds`), a `HierarchyOrder` (`GameWorld`), and a plain `Int`
  (`GameWorld.splashTick: 0`). The `match field { case ComponentTable(any) {…} case
  EntityAllocator {…} case StringMap(any) {…} … }` needed to emit the right
  initialiser per kind is plausibly as much code as the three explicit constructors,
  plus a large amount of new compiler machinery.
- **No real need surfaced** (the gate was "only if #760/#768/#771 show a real need").
  The renderer→ECS migration (#771 / #793–797) added `World3d` and used the explicit
  `createWorld3d` (6 tables) without friction, and #760's own note called construction
  "the cheapest of the three whole-world ops."

If a world ever grows dozens of same-kind fields *and* a genuine constructor bug
appears, revisit — but the mechanism to reach for then is incremental struct
assembly, a general language feature, not a reflection helper.

## Open questions

- Does a field binding need `match` over its type, or is the binding-type filter
  enough in practice? (Start with the filter only; add `match` if a real need appears.)
- ~~Construction via `fields(Type)`: how the loop body assembles the struct literal.~~
  RESOLVED (#774): decided NOT to build — see the section above.
- Nested structs: does `fields()` recurse, or only one level? (One level; recurse
  explicitly with a nested loop.)
- Inferring the world type `W` from the value argument, so callers write
  `clearEntityComponents(world: uiWorld, entity: e)` without an explicit `W:`.
