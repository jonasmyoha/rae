# Proposal: `global.X` for top-level globals

**Status:** Idea, not committed.
**Date raised:** 2026-05-08, by the language author.
**Scribed and annotated by:** Claude.

## The idea (verbatim from the conversation)

> All globals must be assigned and used through a special object named
> `global`. So using `maxParticles` in Tetris 3D `world.rae` would require
> `global.maxParticles` etc. This would make using globals much more
> clear vs. a local variable name. Have to think what about class
> variables — it is kind of the same as using `this.` on class members?

## Sketch

Today:

```rae
let gridWidth: Int = 10

func initWorld(w: mod World) {
  let grid: List(Int) = createList(initialCap: gridWidth * gridHeight)
  ...
}
```

Proposed:

```rae
let gridWidth: Int = 10

func initWorld(w: mod World) {
  let grid: List(Int) = createList(initialCap: global.gridWidth * global.gridHeight)
  ...
}
```

The bare identifier `gridWidth` inside a function body would no longer
resolve to the module's top-level `let` — it would have to be written
`global.gridWidth`. The same prefix would apply at the assignment side
when the global is mutable: `global.counter = global.counter + 1`.

## Why this is appealing

Rae's stated design rules (CLAUDE.md, `design.md`):

> The language prioritizes:
> - Clear, readable syntax that is easy to reason about
> - Semantics that are explicit, stable, and machine-interpretable
> - Minimal syntactic noise and few special cases
> - Predictable behavior suitable for automated reasoning and transformation

`global.X` lands squarely on **explicit and machine-interpretable**:

1. **You can tell, at the use site, what kind of binding you're touching.**
   `global.gridWidth` is unmistakably a top-level. `gridWidth` could be a
   local, a parameter, or a global — the reader (or AI agent) has to
   know the surrounding context to disambiguate.
2. **No accidental shadowing surprises.** If a function happens to have
   `let gridWidth: Int = 7` for a one-liner calculation, it doesn't
   silently mask the global. Both are reachable.
3. **Symmetry with `this.`** Rae already uses `this.field` for instance
   members inside methods. `global.foo` extends the same disambiguating
   namespace pattern to module-level state. If `this` is "the bag of
   per-instance state", `global` is "the bag of per-program state".
4. **Greppable.** `git grep 'global\.'` finds every reference to
   program-level state. That's currently impossible — bare `gridWidth`
   matches function-local declarations and globals indiscriminately.
5. **Discoverable in tooling.** `global.` is a natural autocomplete
   trigger for "show me everything available at module scope".

## Why it cuts both ways

1. **Verbosity.** Tetris 3D references `gridWidth` in roughly 18 places
   across `world.rae`, `physics.rae`, `render.rae`, and `particles.rae`.
   `gridHeight` appears similarly often. Every one of those becomes
   `global.gridWidth`. That's not a refactor that pays for itself in a
   small example; readability has to win in larger codebases for the
   tax to be justified.
2. **`global` becomes magic.** It's a contextual keyword, not a real
   binding — you can't pass it as an argument or take a reference to it.
   That's fine in isolation, but Rae has been good about avoiding magic
   names so far. `this` is the only precedent.
3. **Consts vs. mutables look identical.** `global.gridWidth` (an
   immutable design constant) and `global.score` (a mutable runtime
   counter) have the same shape. We already lose that distinction at
   the declaration site (`let` for both), but the use site magnifies
   the issue. Could be addressed with a separate `const` keyword later,
   independent of this proposal.
4. **Cross-module scoping question.** Today, an auto-imported `let
   foo: Int = 1` in `world.rae` is reachable as `foo` from `main.rae`.
   Does `global` flatten to a single program-wide namespace, or is
   each module its own `global`? Each-module is closer to how
   Python's `module.attr` works; flat-program is closer to C extern.
   Both are defensible; the choice has to be picked deliberately.
5. **Reads well for keys, awkwardly for everything else.**
   `isKeyDown(key: global.keyLeft)` — fine. `global.gridWidth /
   2.0` repeated thrice in one expression — heavier. The win is real
   in the first form, hard-won in the second.

## How well it fits Rae specifically

**Strong fit:**
- The dual-audience design goal ("easy for humans AND AI agents to
  parse, generate, refactor, and analyze") is essentially the case for
  this proposal. Bare identifiers are ambiguous to both humans skimming
  a file and a static analyzer trying to bind names.
- The "explicit, stable, machine-interpretable" line in `design.md`
  reads almost as a direct argument for `global.`.

**Friction:**
- Rae values "minimal syntactic noise and few special cases". This
  proposal adds a *case* (one extra accessor pattern) that shows up
  *frequently* (every global use). Net noise depends on how
  globals-heavy a typical Rae program is.
- Today's auto-import + bare-name model is genuinely pleasant: you
  drop a sibling file in the directory and start using its names. The
  proposal doesn't break that, but it changes the texture — sibling
  files share a `global` namespace rather than a flat lexical scope.

## Concrete impact on the current codebase

A hand-count from a `git grep` of the current tree:

| Place | Bare-global references that would migrate |
|---|---|
| `examples/97_tetris3d/world.rae` | 7 (`gridWidth` × 4, `gridHeight` × 2, `maxParticles` × 1, plus `gridGet`/`gridSet` indirections) |
| `examples/97_tetris3d/physics.rae` | 8 (`gridWidth` × 3, `gridHeight` × 2, `gridGet`/`gridSet`) |
| `examples/97_tetris3d/render.rae` | 10 (`gridWidth`/`gridHeight` in `drawWell`, `drawLockedGrid`, `drawGhostPiece`) |
| `examples/97_tetris3d/particles.rae` | 4 (`maxParticles` × 3, `gridWidth` × 1) |
| `examples/97_tetris3d/input.rae` | 11 raylib key codes (`keyLeft`, `keyRight`, `keyUp`, ...) — every `isKeyDown(key: keyLeft)` becomes `isKeyDown(key: global.keyLeft)` |
| `examples/97_tetris3d/hud.rae` | 6 (palette colours + font slot/path/size) |
| `tests/cases/392_global_let/main.rae` | 4 |
| `tests/cases/396_global_persistence/main.rae` | 2 |

Roughly 50 use sites in the immediate codebase, mostly trivial
mechanical migrations — but most of them in *new* code I just wrote.
That's a real signal: this proposal would have caught the cases where
I had to mentally jump back to the top of the file to confirm whether
`gridWidth` was a global or a parameter.

## The `this.` parallel

The author's own framing is sharp: this is to globals what `this.` is
to struct fields. Rae already enforces `this.field` inside methods —
omitting `this.` doesn't fall back to a class-scoped lookup; the
identifier just doesn't resolve. The proposed rule for globals would
mirror that exactly:

| Inside a... | To touch... | You write... |
|---|---|---|
| method on `World` | a field on the receiver | `this.score` |
| any function | a top-level `let` | `global.score` |
| any function | a parameter or local | bare `score` |

That's a clean, learnable mental model. It also closes a small Rae
inconsistency: today, `this.field` is *required* for member access,
while bare-name lookup is *allowed* for globals. The proposed rule
makes both kinds of "outer scope" use the same machinery.

## Implementation sketch

The change is small in surface area but touches several layers:

1. **Lexer:** no change. `global` becomes a contextual keyword
   recognised in the parser, like `mod` and `view`.
2. **Parser:** in expression context, `global` followed by `.` parses
   as a special node `AST_EXPR_GLOBAL_REF` carrying the trailing field
   name. Standalone `global` (without `.`) is an error.
3. **Sema:** when resolving `AST_EXPR_GLOBAL_REF`, look up the name
   in the merged module's `AST_DECL_GLOBAL_LET` table; reject if
   missing. When resolving a bare `AST_EXPR_IDENT`, exclude globals
   from the candidate set — only locals/params/functions/types/enums
   resolve.
4. **C backend:** emit `global.gridWidth` as the underlying C global
   name (`gridWidth` — the prefix is stripped at codegen). No runtime
   cost.
5. **VM backend:** same — `global.X` resolves to the same `OP_GET_GLOBAL`
   slot the bare identifier resolves to today.
6. **Diagnostics for the migration:** when a bare identifier matches
   a global-let, emit a hint: "use `global.gridWidth` to refer to the
   top-level let". This lets us ship the change with a deprecation
   period instead of a hard break.

The `c_backend.c` recently grew an `AST_DECL_GLOBAL_LET` emit pass and
the VM compiler has the matching `vm_registry_ensure_global` plumbing —
both wire-ups still apply unchanged.

## Open questions

1. **Scope of `global`:** one program-wide namespace (current implicit
   semantics) or one per module (`world.global.gridWidth` etc.)?
   Per-module fits Rae's existing "auto-import sibling files" model
   better; program-wide is simpler.
2. **Mutables:** is `global.score = score + 1` allowed everywhere or
   only from "owning" code? Probably allowed everywhere for now;
   capability-style restrictions are a separate idea.
3. **Migration strategy:** soft (warn-then-migrate) or hard (next
   minor bump)? Soft is friendlier; hard is decisive.
4. **`global` inside expressions:** `let g: Int = global.gridWidth` is
   obvious. What about as a function argument or in a struct literal?
   Should be allowed everywhere.
5. **`const` vs `let`:** orthogonal but related. If we're touching the
   global use-site, it might be a good time to also distinguish
   compile-time constants from mutable state at the declaration site.
6. **What happens to existing tests?** ~50+ use sites across
   examples/tests would migrate. With a soft deprecation window, the
   migration can be done lazily; with a hard break, we'd update them
   in one commit alongside the parser change.

## Honest recommendation

I'd ship it, with two qualifiers:

- **Per-module `global`**, not program-wide. Matches the auto-import
  model that's already in place; users of `world.foo` will still write
  `global.foo` from inside `world.rae`, and `world.foo` from outside.
  (Or equivalently, just `foo` from outside since cross-module is
  already explicit at import time.)
- **Soft migration:** start by emitting a hint-level diagnostic when
  a bare ident resolves to a global, with the suggested replacement.
  Migrate the stdlib + examples, then flip to a hard error in a later
  release.

The most convincing piece of evidence to me is the table above. I
wrote `gridWidth` and `maxParticles` 50+ times across this round of
work, and on at least three occasions I had to scroll back to
`world.rae` to remind myself whether `gridWidth` was a global or a
parameter. `global.gridWidth` would have removed that friction at the
cost of seven more characters per reference.

## Cross-references

- `design.md` — language design rules cited above.
- `tests/cases/396_global_persistence/main.rae` — the smallest
  exercise of globals today.
- `examples/97_tetris3d/` — the largest exercise of globals today.
- `compiler/src/c_backend.c` (the recent `AST_DECL_GLOBAL_LET` emit)
  and `compiler/src/main.c` (the `build_vm_output` registry fix)
  for the compiler-side current state.
