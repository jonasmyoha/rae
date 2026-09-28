# Binding modes

Status: **design, decisions settled 2026-09-29, nothing implemented.**
Branch: `bindings`.

Origin: the canvas-hover leak (#13186639) showed that today's ownership
rules put the same idea in three places (`=`, `=>`, `own x`) and that the C
backend decides ownership with flags flipped during emission. This document
replaces those rules with one matrix. The drop/cascade machinery in
`docs/ownership-model.md` stays; its syntax sections are superseded here.

## The model

Every place a value lands is a **receiver**, and every value that lands is a
**source**. What happens is read off one table.

Two kinds of receiver:

- **A declared name** — a parameter, a `let`/`var`/`const`, a return type —
  carries a mode: `own`, `copy`, `view` or `mod`, written right after the
  name (or after `ret`). The binder is `:`. A bare type here is an error.
- **A value position** — an `=` assignment, a struct-literal field, a list
  element — carries no mode. It copies unless the source is marked `own`.

Five kinds of source: an owned name `x`; `own x`; `copy x`; an alias or a
field (`v`, `a.b`, `items[i]`, a `const`); a new value (a call result, a
literal, an operator expression).

| source ↓ / receiver → | `own T` | `copy T` | `view T` | `mod T` | value position |
|---|---|---|---|---|---|
| owned name `x` | **move**, x ends | copy | alias | alias, x must be `var` | copy |
| `own x` | move, asserted | error | error | error | **move**, x ends |
| `copy x` | copy | copy | error | error | copy |
| alias or field | **copy** | copy | alias | alias, source must be `mod` or owned | copy |
| new value | taken | taken | alias to the end of the statement | error | taken |

Reading the columns:

- **`own T`**: the receiver takes. An unmarked owned name is moved and ends;
  `own x` says the same and refuses any later use; `copy x` keeps the name;
  an alias or a field is never taken, so it is copied.
- **`copy T`**: the receiver promises never to take, whatever the caller
  writes; `own x` into it is an error.
- **`view T` / `mod T`**: an alias, fixed for the name's life. A temporary
  may be viewed for the statement it appears in; it cannot be modified
  (nothing could observe the write).
- **value position**: `=` copies. `= own x` moves. `Holder { label: name }`
  copies, `Holder { items: own items }` moves.

## What it looks like

```rae
let a: own Holder: makeHolder()      # new value: taken
let b: copy Holder: a                # copy; a stays
let c: own Holder: a                 # move; a ends
let v: view Holder: b                # alias
let n: copy Int: settings.count      # a field: copy (own would be an error)
let e: own Int: 5

func store(holder: own Holder)       # takes: the caller decides move or copy
func snapshot(holder: copy Holder)   # never takes: the caller's name always survives
func inspect(holder: view Holder)
func step(world: mod UiWorld)
func makeLabel(seed: view Int) ret own String
func labelOf(holder: view Holder) ret copy String   # returns a field
func firstItem(holder: view Holder) ret opt view String

store(holder: a)                     # move; a ends
store(holder: own a)                 # move, and any later use of a is an error
store(holder: copy a)                # copy; a stays
store(holder: v)                     # an alias: copy
snapshot(holder: own a)              # ERROR: snapshot never takes
entities.add(value: entityId)        # entityId is a view: copy

type Holder {
  label: String                      # a field has no mode: the struct owns it
  items: List(String)
}
ret Holder { label: other.label, items: own items }   # copy; marked move
holder.label = name                  # copy; name stays
app.document = own newDocument       # move
count = count + 1                    # a new value

loop let row: view ListViewRow in rows { ... }
loop var i: own Int: 0, i < n, ++i { ... }
if let inner: view Inner: outer.maybe { ... }
if let row: opt view ListViewRow: rows.viewAt(index: i) { ... }
```

Errors, each with its fix:

```rae
let x: Holder: makeHolder()          # bare type: say own / copy / view / mod
let x: own String: other.label       # a field is not taken: copy or view
let x: own Holder: v                 # an alias is not taken: copy or view
let x: view Holder: makeHolder()     # an alias to a temporary cannot outlive the statement
step(world: readOnlyWorld)           # a view passed to mod
v = b                                # v is a view: aliases are fixed at binding
log(c)   # after `store(holder: own c)` or `store(holder: c)`: c has ended
func f(holder: Holder)               # bare parameter: say the mode
ret holder.label   # under `ret own String`: declare ret copy String
```

## Rules the table does not show

- **A `copy` name is an owned name.** After `let b: copy Holder: a`, `b`
  is in the first row of the table like any other owned name.
- **`let` binds once. A `var` is assigned with `=`**, which copies (or moves
  with `= own x`). A `var` that has ended (moved from) is assigned again
  with `=` and is live from there: "ended" means empty until assigned.
- **A transferred name cannot be read** on any path that reaches the
  transfer. Sema rejects it with the line of the transfer.
- **A `const` is an alias-or-field source**: `copy` or `view`, never `own`,
  never `mod`.
- **A call returning `view T` / `mod T` / `opt view T` yields an alias**,
  never a new value: `let list: view ListView: componentView(...)`.
- **`opt` is always outermost.** `opt T`, `opt view T` (maybe an alias: a
  nullable pointer, what `viewAt` returns), `opt mod T`. There is no
  `view opt T` / `mod opt T`: to narrow a stored optional, narrow it
  directly (`if let inner: view Inner: outer.maybe`); to clear or fill a
  slot, the owning type writes the function (`takeInner(outer: mod Outer)
  ret own Inner`).
- **Aliases between aliases**: `view` → `view`, `mod` → `mod`, `mod` →
  `view` are fine; `view` → `mod` is an error. A field of a `view` cannot
  be passed to `mod`.
- **Canonical modes for a new value**: `own` on a `let`/`var`, `ret own T`
  on a return type; `copy` only where the source is a place. The migration
  tool and the diagnostics use this choice; the formatter never changes a
  mode.
- **A copy of a temporary is elided.** `let x: copy T: makeT()` and `let x:
  own T: makeT()` both mean what they say; the compiler moves the
  temporary in both. Implementation, not a rule.
- **Copies are visible from the line.** A copy happens where `copy` is
  written, where a value position takes an unmarked source, or where an
  `own`/`copy` receiver takes an alias or a field. Nowhere else, and never
  depending on code below the line.
- **`=>` goes away.** `let v: view T: x` replaces `let v: view T => x`,
  also in `if let`.

## Migration (lib + examples, 122K lines, measured 2026-09-25)

| form today | count | becomes |
|---|---|---|
| `let` / `var` / `const` with `=` | ~10,700 | `name: mode T: value`; `own` for a new value, `copy` for a place (sema names the fix) |
| `let x: view/mod T => place` | 639 | `let x: view/mod T: place` |
| `if let x: T = call()` | 672 | `if let x: own T: call()` |
| `if let x: view T => opt` | 113 | `if let x: view T: opt` |
| `= own x` | 33 | unchanged |
| `field: own x` in literals | 476 | unchanged |
| call-site `own x` | ~60 | unchanged (now an assertion) |
| `ret own x` | 14 | `ret x`; the return type says `own` |
| parameters `copy T` / `own T` | 912 / 56 | unchanged; `copy T` now means "never takes" |
| parameters with a bare `T` | ~218 | `copy T` (today's meaning) or `own T`; the author decides |
| `ret T` | most functions | `ret own T`, or `ret copy T` where a `ret` returns a field |
| `=` assignments | 5,800 | unchanged; `=` copies today too |
| `opt view T` | 8 | unchanged |

Every rejected line gets a diagnostic naming the rewrite, so the sweep is
compiler-driven. The mechanical forms (`=>` to `:`, `ret own` to `ret`, the
`=` to `:` in declarations) can be a one-off tool under `compiler/tools/`.

## Implementation

1. **Sema owns ownership.** A per-function pass computes, per name and per
   control-flow path, live / ended / alias, and rejects everything in the
   error list above. It emits a per-statement table the backend reads
   (drop here / ended on this path). Done first, on TODAY's syntax,
   replacing `local_moved[]` / `local_struct_owns_heap[]`; the emitted C
   for the suite must be byte-identical before and after. This fixes the
   bug class behind #13186639 on its own.
2. **Parser.** `name: mode Type: value` in declarations, modes on return
   types, `=>` removed, bare types rejected, `opt` outermost only. Old
   forms parseable only under a flag the migration tool uses, then deleted.
3. **Backend.** Copies, moves and drops from the phase-1 table; the
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
- Before any of this: the cold-read test. An agent with no history rewrites
  `examples/121_ui_editor/inspectorSystem/Panels.rae` from this document
  alone and reports every place a rule bit or a question arose. Each
  question is a gap in this document, not in the file. (A first run, against
  the three-rule draft, produced twelve questions; every one is now
  answered above.)
