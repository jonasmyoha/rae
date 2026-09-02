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

- **Synthesize a world from its component set.** Every world today hand-declares each
  `ComponentTable(T)` field, repeats it in `createX()`, and adds per-component setters.
  A derive/attribute (`@component`, `@world { A, B, C }`) that generates the table
  fields, the constructor, and the registry entries would remove the single largest
  chunk of ECS boilerplate. `UiWorld`, `World3d`, and `GameWorld` are all this same
  pattern typed out by hand.
- **Component registration / reflection `(partly landed #717)`.** The registry maps
  component name ↔ table + flags. Extend toward auto-registration so a component
  declares its own serialize/replicate/editor flags at definition, not in a separate
  table.

## Data modelling

- **Lightweight tuples / multiple return.** Repeatedly hit "can't return `(cellX,
  cellY)` / `(transform, active)`" — had to thread a struct or split into two funcs.
  Cheap multi-return would simplify placement, spatial helpers, and query results.
- **Data-carrying enums / tagged unions.** `PropKind` is an `Int` discriminator matched
  with an if-ladder; a real sum type would make kind-dispatch exhaustive and
  type-safe, and pairs naturally with query-by-variant.
- **Zero-field tag structs `(landed #751/#752)`.** `type FooTag {}` as a marker
  component + `addTag`/`hasTag`/`queryTagged`. Keep; it is how a "kind" if-ladder
  becomes a query.

## Namespacing & modules

- **Per-module type namespacing / qualified type references.** Same-named types across
  modules collide confusingly: `Scene` already exists in lib/ui, and a second 3D world
  named `World3d` would collide with `lib/scene3d` for any file importing both. Being
  able to say `scene3d.World3d` vs `ui.Scene` (types, not just functions) would let two
  domains keep the natural name.

## Iteration-order & lifecycle guarantees

- **Documented stable dense-iteration order.** Migrations rely on "dense order ==
  insertion order," which holds *until* a `componentRemove` swap-remove reorders the
  table. Systems that need a stable order (render submit order, pool slots) currently
  depend on an undocumented invariant. Either guarantee stable order, or provide an
  explicit ordered view, so this can't break silently.
- **Generational entity recycling `(landed #703/#704)`.** `EntityAllocator` recycling
  freed index slots + bumping generation is what lets pools recycle entities instead of
  scale-to-0 culling (terrain #741). Keep; it is the backbone of entity pooling.

## Systems & scheduling

- **First-class systems / schedule `(partly landed: Schedule, resources, EventQueue)`.**
  A system that declares its component reads/writes could be ordered automatically,
  checked for conflicts, and parallelised — turning "a function I remember to call in
  the right phase" into a language-level concept. The manual per-frame call order in
  the examples is the seam a scheduler would own.
