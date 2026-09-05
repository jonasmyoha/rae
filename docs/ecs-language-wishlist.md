# Rae language features for a world-class ECS

Goal: make Rae the best language for building ECS systems, and make ECS a natural,
low-boilerplate architectural pattern in Rae.

This is a **living wishlist**, seeded from the real friction hit while migrating the
example apps to ECS (#706–#741: the generic `lib/ecs`, example 112 `World3d`, example
114 `GameWorld` — hero/crowd/parts/terrain as entities). Each bullet is a concrete
pain point or a leverage point observed in actual migration work, not speculation.
Mark items `(landed)` when the feature ships. Add a new bullet only when a task reveals
a feature that is not already listed here.

## Element mutation & aliasing (the #1 ECS ergonomic)

- **Write-through element access `componentMod` `(landed #721)`.** The single most
  recurring ECS pain was "Rae Lists / tables hand back a *value*, so you copy-out →
  mutate → set-back." `componentMod` returning a live `mod T` removed the churn in
  crowd movement, per-member animation, and terrain placement. Keep this first-class
  and cheap; it is what makes "drive entities in place via queries" possible.
- **Disjoint field borrows must be guaranteed + documented.** Migrations routinely
  need `mod` on two component tables of one world at once (e.g. `world.controllers`
  and `world.animStates`), and a `mod` element ref held across several calls
  (`updateWalkerMovement` then `groundWalker` on the same borrowed controller). It
  works today, but the rules are folklore — a clear, permissive, documented story for
  borrowing disjoint fields of a struct/world removes uncertainty at every system.
- **Guard against implicit heap-struct copy.** `var w = asset.world` on a heap-owning
  struct did a *shallow bitwise copy* (shared component-table pointers) → double-free /
  SIGABRT that only showed up at teardown after a screenshot already rendered. Moving
  or aliasing a heap-owning value out of a field should require an explicit move, or be
  a compile error — never a silent shallow copy.

## Generics & queries

- **Multi-type-param generic inference `(landed #707)`.** Foundational: `query2`/
  `query3` joining `ComponentTable(A) × ComponentTable(B)` and inferring every type
  param. Without it there is no ergonomic join.
- **Richer query combinators.** `queryTagged` covers "data table filtered by a tag,"
  but real systems want: `with A, without B`, *optional* components, and
  change-detection ("only entities whose A changed since last run"). A small,
  composable query builder would replace hand-rolled per-kind scans (terrain draw
  currently dispatches on a `kind` field inside one loop).
- **Ergonomic query iteration.** `for (entity, mod a, mod b) in query(world, A, B)`
  instead of `componentCount` + `componentEntityAt(i)` + `componentDataAt(i)` +
  `componentGet`. The index-plus-accessor form is verbose and easy to get subtly wrong.

## World / archetype boilerplate

- **Synthesize a world from its component set + reflect over its fields.** Every world
  today hand-declares each `ComponentTable(T)` field, repeats it in `createX()`, adds
  per-component setters, AND re-lists every table in each whole-world op (destroyEntity's
  per-table `componentRemove`, worldToJson's per-table `addComponentTable`) — because Rae
  has no field reflection and no function values, so nothing can hold a world's
  heterogeneous tables as data. A compile-time "for each `ComponentTable(_)` field of this
  struct" reflection (feeding a `createX` / `clearEntity` / `worldToJson` generated once,
  generically) would remove the single largest chunk of ECS boilerplate — and it is the
  ONLY thing that makes "add a component, edit nothing else" possible. `UiWorld`,
  `World3d`, and `GameWorld` are all this same pattern typed out by hand.
  - **Design chosen → `docs/compile-time-reflection.md`.** Reuse the existing
    collection loop with a compile-time sequence after `in`, unrolled by the compiler;
    the binding's explicit type is the field filter, `any` the type wildcard, and slot
    metadata is a compile-time plain function: `loop let table: mod ComponentTable(any)
    in fields(world) { componentRemove(this: table, entity: entity) }`,
    `fieldName(table)`, `fields(UiWorld)` for the type. Rule: all reflection data is a
    compile-time plain function called with parens (UFCS `world.fields()` is fine) —
    never a phantom member like `world.fields` / `table.fieldName`. Parser unchanged;
    sema dispatches on the iterable; codegen unrolls. Not `#run`, not runtime type
    info, not attributes. Status: values + generic-W landed (#772/#773); first
    consumer wired in #760. The CLEAR/SERIALIZE halves are done; the CONSTRUCTION
    half (`createX` via `fields(Type)`) was assessed and **declined** (#774) — a
    forgotten table is already a compile error there, so reflection buys no safety,
    and it would need a new incremental struct-assembly construct for the least gain
    (full reasoning in `docs/compile-time-reflection.md`).
  - **Element type-name reflection `typeName(T)` (new, surfaced by #760).** `fieldName`
    gives a table's FIELD name (`positions`), but the serializer's registry is keyed by
    the COMPONENT type name (`Position`). With only `fieldName`, the registry-gated
    `loop ... in fields(world) { addComponentTable(name: fieldName(table)) }` forces the
    registry to be re-keyed by field name (done for example 116). A compile-time
    `typeName(table)` / element-type-name primitive (the name of the `T` in
    `ComponentTable(T)`, folded to a String literal like `fieldName` is) would let the
    loop key by the canonical component name and keep ONE registry vocabulary shared
    with the human-authored `.raescene` loader (#768) — instead of two (`Rect` vs
    `rects`). Same rules as `fieldName`: a plain compile-time function, parens, no sigil.
  - **Spelling constraint (maintainer):** spec this as a REAL keyword, a plain
    function, or a compile-time construct — NEVER an `@`-sigil attribute
    (`@derive`/`@component`/`@world`) or the `#[...]` variant. The reasoning (fuller
    version in AGENTS.md → "NO `@`-sigil keywords"): there is no real "metadata vs
    logic" line to justify the sigil — the same concept is `@Override` in one language
    and bare `override` in another, and `const`/`public` could just as well be
    `@const`/`@public`, so the category is fake. The split is arbitrary even within one
    language (Java sigils `@Override` but not `public`; Rust `#[derive]` but not `pub`),
    making the programmer memorise a second, parallel vocabulary for no rule. The ONE
    place a sigil is earned is an OPEN, user-extensible set (Python decorators, Rust
    macros) as a namespace escape hatch — and Rae deliberately does NOT want that
    open-macro extensibility (it fights determinism/analyzability), so it never reaches
    the one justified case. One bare-keyword vocabulary, always.
  - **Not a compiler builtin either.** A bespoke one-use `clearEntityComponents`
    special-case synthesised in sema/c_backend is off the table too — that is the
    forbidden hand-added C-backend feature for a single purpose. The capability must be
    expressible IN Rae via a GENERAL reflection/derive mechanism (used to write
    `createX`/`clearEntity`/`worldToJson` once, generically), not a one-off codegen path.
- **Component registration / reflection `(partly landed #717)`.** The registry maps
  component name ↔ table + flags. Extend toward auto-registration so a component
  declares its own serialize/replicate/editor flags at definition, not in a separate
  table.

## Data modelling

- **Lightweight tuples / multiple return.** Repeatedly hit "can't return `(cellX,
  cellY)` / `(transform, active)`" — had to thread a struct or split into two funcs.
  Cheap multi-return would simplify placement, spatial helpers, and query results.
- **Data-carrying enums / tagged unions (sum types) — DECIDED: Rae will NOT have
  them.** This was proposed (briefly queued as #808) on the usual grounds — `PropKind`
  is an `Int` discriminator matched with an if-ladder, and a sum type would make
  kind-dispatch exhaustive and tie a variant's *data* to its tag so reading the wrong
  variant's fields is a compile error. It was declined as a definitive decision, for
  three reasons that hold together; do not re-propose it without a concrete case that
  defeats all three.
  - **It is not data-oriented — the memory layout is wrong for ECS.** A tagged union
    is sized to its LARGEST variant plus a tag plus padding, so every small variant is
    padded up to the big one. Worse, a *collection* of them is an array of mixed,
    max-sized slots: iterating "all circles" has to touch every slot including the
    rects, so you can never get a dense, homogeneous, cache-friendly stream. That is
    exactly the array-of-structs "bag of mixed things" layout data-oriented design
    exists to get away from. The ECS answer already gives the right layout for free:
    each component type is its own packed dense `ComponentTable`, so "all circles" IS
    a contiguous array of only circles. A sum type optimises the programmer's
    convenience of "one value, many shapes" at the direct cost of the layout a
    performance engineer actually needs.
  - **The ECS *is* the variant mechanism, so a sum type is a competing second way.**
    In an ECS, "a kind with data" is idiomatically not a sum type but separate
    **components + tags** on entities: "which variant is this" becomes "which
    component does the entity have," and dispatch becomes a **query**. The safety a
    sum type would add — you cannot read a variant's fields unless you established
    that variant — is already there: `componentGet` returns `opt`, the component is
    present or it isn't. Adding sum types would give Rae a second vocabulary for
    variants that fights the architecture the language is built around, which is the
    precise kind of overlapping feature "few special cases" refuses. The `PropKind`
    if-ladder that motivated the proposal is therefore an ECS *smell*, not a missing
    type feature: it should become tag components dispatched through queries (the
    N-ary / combinator query work in #807), not a new type kind.
  - **Rae already has the one sum type that pays for itself: `opt T`.** `some`/`none`
    is a two-variant tagged union, and it covers the overwhelmingly common case
    (result-or-absent, present-or-not) with a dedicated construct instead of a general
    one. That is the minimalist move and it works. The residual cases a general sum
    type would serve are values that are NOT entities and so cannot live in a table —
    a JSON node, a parsed AST node — and for those a struct-with-kind is acceptable
    (library-internal, the padded fields do not matter) or `opt` suffices. None of
    them justifies a whole new type kind touching parser, type system, sema, the C
    backend, and drop codegen for heap-owning variants.
  - Net: sum types are a language-nerd feature, not one a performance-oriented
    engineer needs in a minimalistic ECS language. Variants are components + tags +
    queries; `opt` is the blessed two-way case; keep it that way.
- **Zero-field tag structs `(landed #751/#752)`.** `type FooTag {}` as a marker
  component + `addTag`/`hasTag`/`queryTagged`. Keep; it is how a "kind" if-ladder
  becomes a query.

## Namespacing & modules

- **Per-module type namespacing / qualified type references.** Same-named types across
  modules would collide confusingly: `Scene` lives in lib/ui, and if a second module
  ever wanted a bare `Scene` (or `World`) the two could not coexist for a file importing
  both. Being able to say `ui.Scene` vs `otherModule.Scene` (types, not just functions)
  would let two domains keep the natural name. This is the reason the #770 naming
  decision (see `docs/naming-conventions.md`) keeps `UiWorld` / `World3d` / `Scene`
  rather than forcing a symmetric `World2d`/`Scene2d` rename: namespacing, not renaming,
  is the right fix for name collisions.
- **Cyclic imports across modules `(landed #743)`.** The loader rejected any import
  cycle, which would have forced a shared "types" dumping ground or dependency inversion
  to fold mutually-referential ECS systems. Since Rae merges every module into one unit
  before sema, cycles are safe; the loader now allows them.
- **Real module encapsulation for project files.** The deeper #743 finding: Rae
  auto-scans *every* `.rae` file under the project root into ONE compile + visibility
  unit, so moving a system into a `FooSystem/` folder does NOT create an import wall —
  every project file still sees every other, no `import` needed. Folders are namespaces
  for *organisation*, not encapsulation. That is why the App/system coupling was a
  PARAMETER-level problem (systems taking the God-object `App`), fixable by passing narrow
  state — the module system neither caused nor cured it. To make a system's dependencies
  *enforced* (it can name only what it imports), Rae would need opt-in module
  encapsulation (e.g. a package that is NOT auto-opened to siblings). Without it, "clean
  module boundaries" for an in-project ECS live only in discipline, not the compiler.

## Iteration-order & lifecycle guarantees

- **Documented stable dense-iteration order.** Migrations rely on "dense order ==
  insertion order," which holds *until* a `componentRemove` swap-remove reorders the
  table. Systems that need a stable order (render submit order, pool slots) currently
  depend on an undocumented invariant. Either guarantee stable order, or provide an
  explicit ordered view, so this can't break silently.
- **Generational entity recycling `(landed #703/#704)`.** `EntityAllocator` recycling
  freed index slots + bumping generation is what lets pools recycle entities instead of
  scale-to-0 culling (terrain #741). Keep; it is the backbone of entity pooling.
- **Generic `world.despawn(entity)` that clears every component table.** Despawn today
  is manual: `componentRemove` from each specific table (physicsBody, transform, …) then
  `freeEntity`, per site (net/physics seams #746). Forget one table and a recycled index
  reads stale data. A structural op that, given the world's registered component set,
  removes an entity from *all* its tables in one call (and ideally detaches hierarchy
  links) would make despawn safe by default. This is exactly the first worked example in
  `docs/compile-time-reflection.md` (`clearEntityComponents` as a
  `loop let table: mod ComponentTable(any) in fields(world)`); it does NOT need the
  registry to enumerate tables — the compiler does, from the world's fields.

## Systems & scheduling

- **First-class systems / schedule `(partly landed: Schedule, resources, EventQueue)`.**
  A system that declares its component reads/writes could be ordered automatically,
  checked for conflicts, and parallelised — turning "a function I remember to call in
  the right phase" into a language-level concept. The manual per-frame call order in
  the examples is the seam a scheduler would own.

## Ownership & the no-globals rule

- **Implicit context parameter (Odin/Jai `context`)** — would let a profiler or theme
  reach leaf functions without threading. #764 finished the no-globals migration by
  hand: every cross-cutting singleton (Profiler, GrassCompute, the DeferredRenderer pass
  caches, the UI `UiTheme`) became a field on an owner (App / world / renderer resource)
  and was spelled at every hop from the owner down to the leaf — the theme alone touched
  the whole paint + scene-deserialiser chain across lib/ui and three example apps. A
  language-level ambient context (a `context`-style scoped binding threaded implicitly by
  the compiler, one keyword, no `@`-sigil) would collapse that mechanical mega-diff into
  an edit. **Against:** Rae's promise is that what a function needs is in its signature;
  Zig rejected implicit context for exactly that reason (readability / no hidden inputs a
  tool can't see); Bevy threads `&Theme`/`Res<T>` through helpers rather than hiding it.
  Whatever is decided, decide it deliberately as a language question — not as fallout from
  #764. Design space, not a concrete proposal; if pursued, spell it as a real keyword in
  the `func`/`type` namespace, never an attribute.
