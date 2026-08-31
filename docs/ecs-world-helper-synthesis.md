# Compiler-synthesized per-World ECS helpers — decision note (#756)

**Status:** investigation complete; leak fixed; a recommended prototype design
is specified below and queued for approval (#757). This note is the "documented
decision" the task's DoD allows.

## The problem

A `World` struct in Rae holds one `ComponentTable(T)` field per component type
(see `lib/ui/ecs.rae:UiWorld`, ~72 tables). Adding a component today touches
**four** places:

1. the struct field — `foos: ComponentTable(Foo)`
2. the `createUiWorld()` default-construct — `foos: createComponentTable(Foo)`
3. the `destroyEntity` sweep — `componentRemove(this: world.foos, entity: entity)`
4. the registry dispatch arm — `registerComponent(...)` in `registry.rae`

Places 3 and 4 are pure boilerplate mechanically derivable from the field list,
and **they drift**. Concrete evidence found during this investigation: seven
declared `UiWorld` tables — `cornerRadiuses`, `hoverScales`, `gradientFills`,
`backdropImages`, `opticalAligns`, `layerRoots`, `layerRefs` — were **missing**
from `destroyEntity`, so every destroyed entity leaked its rows in those tables.
That is the exact failure mode hand-maintained field-parallel boilerplate
produces. (Fixed in this commit as a prerequisite; see below.)

Rae **cannot** close this in a library: it has no reflection and no
heterogeneous field iteration, so you cannot write "for each `ComponentTable`
field of `world`, `componentRemove` it" — each field has a different `T`. The
compiler is the only lever, which is what this task investigates.

## Why not auto-generate a `destroyEntity` free function

The most literal reading — the compiler detects "a struct with `ComponentTable`
fields" and *invents* a `destroyEntity(world, entity)` free function — was
considered and **rejected**:

- **It collides.** `UiWorld` already defines `destroyEntity` by hand; so would
  the GameWorlds. Auto-generation means silently *replacing* a hand-written
  function, or a duplicate-definition error.
- **It is un-greppable.** A reader (human or AI) who sees `destroyEntity(world,
  e)` and searches for `func destroyEntity` finds nothing — the definition
  exists only inside the C backend. This directly violates Rae's core value:
  *easy for humans and AI to read, parse, and analyze* (`AGENTS.md`). Implicit,
  invisible top-level functions are exactly the "clever ambiguity" the language
  is designed to avoid.
- **The name is too broad to reserve.** `destroyEntity` as a structural,
  globally-injected builtin has a large collision surface across app code.
- **Teardown is not purely mechanical.** `destroyEntity` also does
  `freeEntity(world.allocator, entity)` and carries an *ordering* contract
  (Children removed relative to Parent). A fully-synthesized function has to
  encode policy the author currently states explicitly.

## Decision — synthesize only the un-writable part, explicitly

Synthesize a **narrow, explicitly-called compiler builtin** that performs *only*
the heterogeneous field sweep — the one thing Rae cannot express — and leave the
policy (allocator free, ordering, the function's public name) in ordinary,
greppable Rae:

```rae
func destroyEntity(world: mod UiWorld, entity: view EntityId) {
  clearEntityComponents(world: world, entity: entity)   # compiler builtin
  freeEntity(this: world.allocator, entity: entity)
}
```

`clearEntityComponents(world: mod W, entity: view EntityId)` is a builtin
recognized in sema exactly like `sizeof`/`Array` (`sema.c` AST_EXPR_CALL /
IDENT site, ~line 2493). It type-checks arg0 to a struct with `ComponentTable`
fields and arg1 to `EntityId`, then lowers to a synthesized C function
`rae_clearEntityComponents_<W>` whose body mirrors the existing per-struct
`rae_drop_<T>` field loop (`c_backend.c:1822`): one `componentRemove_<T>` call
per `ComponentTable(T)` field, in field-declaration order, reusing the
already-emitted `componentRemove` specializations.

Why this shape:

- **Boilerplate place 3 drops to zero.** Adding a component = declare the field;
  the sweep picks it up. The drift bug class is designed out.
- **Everything stays explicit and analyzable.** `destroyEntity` remains real,
  greppable Rae. The one line the reader can't chase into is a *named builtin* —
  same contract as `sizeof`/`drop` — not an invisible whole function. The World
  type is named at the call site, so there is no global-name collision.
- **Policy stays with the author.** `freeEntity`, ordering, and the public name
  are visible Rae, not compiler policy.
- **Smallest spec-in-the-spirit-of-drop.** It matches the existing
  drop/copy/eq/toJson synthesis: the compiler emits the field-parallel body; the
  language surface gains one narrow verb, not an implicit declaration.

Registry population (place 4) is a natural **follow-on** using the same field
scan: a builtin `registerWorldComponents(registry, world)` emitting one
`registerComponent` per table with flags read from per-field annotations. Kept
out of this prototype because component flags (SERIALIZE/REPLICATE/EDITOR_ONLY)
are not derivable from types alone and need a declaration mechanism — a separate
design step.

### Rejected alternatives, briefly

- **Auto free-function** — collision + un-greppable (above).
- **Marker syntax** (`func destroyEntity(...) synth`) — new syntax; forbidden.
- **Empty-body detection** (fill an empty `destroyEntity` by signature) — magic
  by signature; fragile and invisible.
- **Pure library** — impossible without reflection.

## Approval gate

`clearEntityComponents` is a **new language builtin** = a semantic addition.
`AGENTS.md` requires human approval before introducing new semantics, and the
human owns language design. So the implementation is **queued (#757) pending
approval**, not merged unilaterally. This note + the recommendation is the
approval request. The prototype is scoped to: sema recognition + type-check,
`c_expr.c` lowering, `c_backend.c` synthesis, migrate `UiWorld.destroyEntity` to
the two-line form, verify byte-identical component clearing + suite green; then
#722 and the GameWorlds adopt it.

## Shipped in this commit (unblocks the above)

The seven-table `destroyEntity` leak is **fixed** (`lib/ui/ecs.rae`): all 72
declared `UiWorld` `ComponentTable` fields are now cleared on destroy. This is a
real correctness fix and it makes the hand-written sweep a *correct* reference
for the future synthesized version to match byte-for-byte ("identical in
behaviour" is only well-defined against a correct baseline).
