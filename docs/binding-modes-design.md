# Binding modes

Status: **design, settled at the rule level 2026-09-28, nothing implemented.**
Branch: `bindings`.

Origin: the canvas-hover leak (#13186639) showed that today's ownership
rules put the same idea in three places (`=`, `=>`, `own x`) and that the C
backend decides ownership with flags flipped during emission. This document
replaces those with three rules. The drop/cascade machinery in
`docs/ownership-model.md` stays; its syntax sections are superseded here.

## The three rules

1. **A declared name carries its mode.** A `let` / `var` / `const`, a
   parameter and a return type each say one of `own`, `copy`, `view`, `mod`
   right after the name (or after `ret`). The binder is `:` everywhere. A
   `let` binds once. A bare type in one of these positions is an error.
2. **`=` copies.** In a value position (an `=` assignment, a struct-literal
   field, a list-literal element) an unmarked value is copied; `own x` marks
   the one transfer. There are no other value-side words.
3. **`own` takes only from a new value or a whole owned name.** The whole
   name ends. A field, an element or an alias (`view`/`mod`) is never taken
   from; write `copy` or `view` there.

Everything below is these three rules applied.

## What it looks like

```rae
let a: own Holder: makeHolder()      # a new value
let b: copy Holder: a                # an independent copy; a stays
let c: own Holder: a                 # a ends
let v: view Holder: b                # an alias, fixed for v's life
var count: own Int: 0
let e: own Int: 5
let f: copy Int: 5                   # legal: a copy of a literal is a value

func consume(holder: own Holder)     # the call site writes nothing;
func keep(holder: copy Holder)       #   the parameter is the declared name
func inspect(holder: view Holder)
func step(world: mod UiWorld)
func makeLabel(seed: view Int) ret own String
func labelOf(holder: view Holder) ret copy String   # returns a field: copy
func firstItem(holder: view Holder) ret view String

type Holder {
  label: String                      # a field has no mode: the struct owns it
  items: List(String)
}

ret Holder { label: other.label, items: own items }   # field copies; own marks the transfer
count = count + 1                    # = copies a new value
current = other                      # = copies other; current's old value is dropped
app.document = own newDocument       # the marked transfer into an existing place
consume(holder: a)                   # a ends: holder is own
keep(holder: a)                      # a stays: holder is copy
ret flat                             # taken as the return type says
```

Errors, each with its fix:

```rae
let x: Holder: makeHolder()          # bare type: say own / copy / view / mod
let x: own String: other.label       # a field is not taken from: copy or view
let x: own Holder: v                 # an alias is not taken from: copy or view
consume(holder: world)               # world is mod: an alias is not taken from
v = b                                # v is a view: aliases are fixed at binding
log(c)   # after `consume(holder: c)`: c has ended
func f(holder: Holder)               # bare parameter: say the mode
ret holder.label   # under `ret own String`: declare ret copy String
```

## Consequences worth knowing

- **Copies are visible.** A copy happens either where `copy` is written on
  a declared name, or in a value position, which means copy by definition.
  Nowhere else.
- **A copy of a temporary is elided.** `let x: copy T: makeT()` and
  `let x: own T: makeT()` both mean what they say; the compiler moves the
  temporary in both. Implementation, not a rule.
- **Transfer into an existing place is `= own x`**, the same marker the
  literal field uses. A setter with an `own` parameter is the other way,
  and the ECS one (`componentSet(data: own T)`).
- **Taking a value out of a struct** is not expressible with a word,
  because a field is never taken from. It is a function on the owner
  (`take(this: mod opt T) ret own T`, or a method that hands the field out
  under `ret own T` by moving it internally). That is intended: a struct's
  contents leave through its own API.
- **`=>` goes away.** `let v: view T: x` replaces `let v: view T => x`, also
  in `if let` (`if let v: view T: opt`; `if let v: copy T: opt` copies;
  `if let v: own T: call()` from a call result only).
- **Loops** follow the same form: `loop let x: view T in list`,
  `loop var i: own Int: 0, i < n, ++i`.
- **`opt` and the mode**: the tree writes `opt view T` in 8 places today.
  Mode-first (`view opt T`) matches every other position; `opt view T` reads
  as "an optional of a view". Cosmetic; decide during implementation.
- **Aliases are fixed.** A `view`/`mod` name is bound once. Writing
  *through* a `mod` alias (`state.count = 1`, `state.items = own items`) is
  an ordinary value-position write into the pointee.

## Migration (lib + examples, 122K lines, measured 2026-09-25)

| form today | count | becomes |
|---|---|---|
| `let` / `var` / `const` with `=` | ~10,700 | `name: mode T: value`; sema says which mode fits (a place source: `copy` or `own`; a new value: `own` or `copy`) |
| `let x: view/mod T => place` | 639 | `let x: view/mod T: place` |
| `if let x: T = call()` | 672 | `if let x: own T: call()` |
| `if let x: view T => opt` | 113 | `if let x: view T: opt` |
| `= own x` | 33 | unchanged |
| `field: own x` in literals | 476 | unchanged |
| `ret own x` | 14 | `ret x`; the return type says `own` |
| parameters `copy T` / `own T` | 912 / 56 | unchanged |
| parameters with a bare `T` | ~218 | `copy T` (today's meaning) or `own T`; the author decides |
| `ret T` | most functions | `ret own T`, or `ret copy T` where a `ret` returns a field |
| `=` assignments | 5,800 | unchanged; `=` copies today too |

Every rejected line gets a diagnostic naming the rewrite, so the sweep is
compiler-driven. The mechanical forms (`=>` to `:`, `ret own` to `ret`, the
`=` to `:` in declarations) can be a one-off tool under `compiler/tools/`.

## Implementation

1. **Sema owns ownership.** A per-function pass computes, per name and per
   control-flow path, live / ended / alias, and rejects: a read after a
   transfer, `own` from a field or an alias, an alias of a new value, an
   assignment to an alias, a bare type. It emits a per-statement table the
   backend reads (drop here / ended on this path). Done first, on TODAY's
   syntax, replacing `local_moved[]` / `local_struct_owns_heap[]`; the
   emitted C for the suite must be byte-identical before and after. This
   fixes the bug class behind #13186639 on its own.
2. **Parser.** `name: mode Type: value` in declarations, modes on return
   types, `=>` removed, bare types rejected. Old forms parseable only under
   a flag the migration tool uses, then deleted.
3. **Backend.** Copies, transfers and drops from the phase-1 table; the
   emission-time move marks deleted.
4. **Formatter.** Canonical layout for the new headers.
5. **Sweep.** lib, then examples, then fixtures, driven by the diagnostics.
   A fixture per error above, each with the outstanding-allocation check
   fixtures 918–920 use.
6. **Docs.** `docs/ownership-model.md` syntax sections point here; AGENTS.md
   "NO type inference" gains the binding grammar; `rae init` scaffold.

## Verification

- Full suite and every example gate green after each phase.
- Fixtures 916–920 unchanged in meaning, rewritten in the new grammar.
- The hover soak (`examples/121_ui_editor/tools/hover-soak.sh`) and the
  run_examples.sh hover-leak check stay at the boot baseline.
- Before any of this: a cold-read test. An agent with no history rewrites
  one real file (`examples/121_ui_editor/inspectorSystem/Panels.rae`)
  from this document alone and reports every place a rule bit or a
  question arose. Each question is a gap in the three rules, not in the
  file.
