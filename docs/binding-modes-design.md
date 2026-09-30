# Binding modes

Status: **decided 2026-09-30: option N of `docs/binding-syntax-survey.md`.**
This replaces the `name: mode Type: value` grammar settled on 2026-09-29,
which is withdrawn (it is in git history). The ownership model (who takes,
who copies, who aliases) is unchanged. Only its spelling went back to the
one Rae already has.

Origin: the canvas-hover leak (#13186639) showed that the C backend decides
ownership with flags flipped during emission. The fix for that is
implementation work (§5). The syntax question it raised, how a binding says
what it does, is answered here: by the binder, which Rae already spells three
ways.

## 1. The decision

Three binders, one meaning each:

| binder | meaning | where |
|---|---|---|
| `:` | a **label**: a name's type and mode, or a slot in *another* namespace that gets a value | parameters, type fields, named arguments, struct-literal fields |
| `=` | a **place here is written now**: copy, or move with `own` | local declarations, assignments, field defaults |
| `=>` | a **name is bound to an existing place**: an alias, no value moves | `let v: view T => place`, `if let v: view T => opt` |

The left side of `f(world: world)` or `{ name: n }` is not a place the caller
can see. It is a parameter of the callee, or a field of the type being built:
a slot defined elsewhere and filled from here. The left side of `x = v` is a
place in this scope, written at this moment. An alias is neither: it names a
place that already exists.

Where modes are written:

- **Parameters always carry one**: `own`, `copy`, `view` or `mod`. This is
  already the rule (bare parameters are a compile error, see
  `docs/ownership-model.md`). It is the contract with the caller: `own`
  versus `copy` says whether the caller's name ends or survives.
- **Locals carry one only when they alias**: `let v: view T => x`,
  `let m: mod T => x`. A value local is `let x: T = v` or `var x: T = v`, and
  what `=` does is read off the right-hand side (§2).
- **Return types carry one only when they hand out an alias**: `ret view T`,
  `ret mod T`, `ret opt view T`. A plain `ret T` returns a value. `own` /
  `copy` on a return would be invisible to the caller, who receives a fresh
  value it owns either way.

```rae
type Holder {
  label: String                      # a field has no mode: the struct owns it
  items: List(String)
  count: Int = 0                     # a field default: a place written at construction
}

func store(holder: own Holder)       # takes: the caller's owned name ends
func snapshot(holder: copy Holder)   # never takes: the caller's name survives
func inspect(holder: view Holder)
func step(world: mod UiWorld)
func makeLabel(seed: view Int) ret String
func labelOf(holder: view Holder) ret String         # returns a field: copied
func firstItem(holder: view Holder) ret opt view String

let a: Holder = makeHolder()         # a new value: taken
let b: Holder = a                    # copied; a stays
let c: Holder = own a                # moved; a ends
let v: view Holder => b              # an alias
let m: mod Holder => c
let n: Int = settings.count          # a field: copied

store(holder: b)                     # own parameter: b ends
snapshot(holder: c)                  # copy parameter: c stays
store(holder: v)                     # an alias into own: copied
holder.label = name                  # copied; name stays
app.document = own newDocument       # moved
ret Holder { label: other.label, items: own items }   # copied; moved

loop let row: view ListViewRow in rows { … }
loop var i: Int = 0, i < n, ++i { … }
if let inner: view Inner => outer.maybe { … }
if let member: EntityId = selection.copyAt(index: k) { … }
```

## 2. The matrix

Every place a value lands is a **receiver**, and every value that lands is a
**source**. What happens is read off one table.

Sources: an owned name `x`; `own x`; an alias or a field (`v`, `a.b`,
`items[i]`, a `const`); a new value (a call result, a literal, an operator
expression). A call that returns `view T` / `mod T` / `opt view T` yields an
alias, never a new value.

| source ↓ / receiver → | `own` parameter | `copy` parameter | `view` / `mod` (parameter or `=>` local) | value position (`=`, literal field, `ret`) |
|---|---|---|---|---|
| owned name `x` | **move**, x ends | copy | alias (`mod`: x must be `var`) | copy |
| `own x` | move, asserted | error | error | **move**, x ends |
| alias or field | **copy** | copy | alias (`mod`: source must be `mod` or owned) | copy |
| new value | taken | taken | `view`: alias to the end of the statement; `mod`: error | taken |

Rules the table does not show:

- **A moved name cannot be read** on any path that reaches the move. A `var`
  that was moved from is live again once assigned.
- **A `const` is an alias-or-field source**: copied or viewed, never taken.
- **`view` → `mod` is an error**, whether in an `=>` binding or when a `view`
  is passed to a `mod` parameter. So is passing a field of a `view` to `mod`.
- **`ret x` of an owned local hands it over**; `ret` of a field copies; `ret`
  of a new value takes it.
- **A copy of a temporary is elided.** Implementation, not a rule.
- **Copies are visible from the line.** A copy happens only where a value
  position takes an unmarked name or field, or where an `own` / `copy`
  parameter receives an alias or a field. It never depends on the type, and
  never on code below the line.

This is today's normative behaviour (`docs/ownership-model.md`: "Do NOT
silently move in ordinary assignments or struct field init", "`own T` …
move / consume", "Returning an owned local … moves it out") written as one
table. §4 lists the cells today's compiler does not yet enforce.

## 3. Why not the alternatives

`docs/binding-syntax-survey.md` has the full comparison (options A–N) and the
same snippet in 18 languages. In short:

- **`name: mode T: value`** (the withdrawn grammar) puts a value after `:`.
  That is the one place in Rae where `:` would not be a label, and in Odin
  and Jai `x: T: v` means a compile-time constant.
- **`=` for named arguments** (option M) buys call/declaration mirroring,
  but a parameter label is not a place in the caller's scope, and the change
  touches most lines of the tree.
- **Option N keeps `README.md`'s refactoring argument literally true**:
  `=` never aliases and copies whatever the type; `=>` always aliases; a move
  is always the written word `own`.

## 4. What the compiler must change

Only what option N makes an error but today's compiler accepts:

1. **Reject a `view` passed to a `mod` parameter** (and a field of a `view`
   passed to `mod`). Today only the `=>` binding is checked (fixture 613).
   There is one known case in the tree: `examples/121_ui_editor`'s
   `syncInspectorPanel` (via `setWidgetRows`) and
   `ClickScript.logInspectorPanel` took `world: view UiWorld` and wrote it.
   Both now take `mod` (fixed in the source; it compiles on today's
   compiler), so turning the check on breaks nothing known. A fixture joins
   613 for the argument case.
   **Done (compiler 0.1.90):** `sema_reject_view_to_mod` checks every
   argument whose parameter is `mod`, and a method receiver whose `this` is
   `mod`; fixture 929. Turning it on found 46 more bindings in `lib/ui` and
   the UI examples that were declared `uiSystems: view UiSystems` (and one
   `view DebugOverlay`) while their callees wrote through them — the
   hierarchy's parent table, list-view pools, hero transitions. They now
   take `mod`.
2. **Confirm the edge cells of §2 against today's compiler** before relying
   on them: an alias or field passed to an `own` parameter copies (it
   compiles today, e.g. `entities.add(value: entityId)` with a `view
   EntityId`, but whether it deep-copies a heap type is unverified); `own x`
   into a `copy` parameter is an error; `ret` of a field copies. Each gets a
   fixture; any cell that disagrees is either a bug fix or a correction to
   this table, decided per cell.
   **Done (compiler 0.1.91), the table unchanged:** every edge cell now
   matches it. Fixture 930 covers the cells that already agreed (a String
   field into `own` copies, `ret` of a field copies, a moved `var` is live
   again once assigned); 931 the ones fixed to match — a List or struct
   FIELD, an alias, a view parameter and a constant handed to `own` are now
   deep-COPIED (a field used to be handed over bit-for-bit and freed twice;
   an alias was a compile error); 932 `own x` into a `copy` parameter, now
   an error.

No parser, formatter or migration work is needed. The spelling does not
change.

## 5. Implementation that is not a language change

**Sema owns ownership.** A per-function pass computes, per name and per
control-flow path, live / moved / alias, rejects everything the matrix
rejects, and emits a per-statement table the backend reads (drop here /
ended on this path). It replaces `local_moved[]` /
`local_struct_owns_heap[]` in the C backend. The emitted C for the suite must
be byte-identical before and after. This is the fix for the bug class behind
#13186639, and it stands on its own: it neither needs nor implies any syntax
change.

## 6. Made possible, deliberately not done

These are legal (or cleaner) under the matrix, but none is required, and
none should be swept for its own sake:

- **Interpolation-as-copy workarounds** (`typeLine = "{entityType}"`,
  `"{tally.name}"`, `"{mixedMarker}"`) and `stringCopy(string: x)` in a
  literal field can become plain `=` / `own x`, because `=` copies. Clean
  them up only in files already being edited, the same phasing as the naming
  rules in AGENTS.md.
- **Moving at last use** (`chromeSystem.inspectorType = own typeLine`) saves a
  copy where the name dies anyway. It is an optimisation to make when it
  matters, not a rule.
- **A call-site `copy x`**, to pass a copy into an `own` parameter and keep
  the name, is not in use anywhere and is not added.

## 7. Open, not decided here

- **Call-site `mod`.** `README.md` says that after a parameter becomes `mod`,
  "every caller reads `process(x: mod track)`". Today a call-site mode is
  optional: it appears in fixture 612 and nowhere in `lib/` or `examples/`.
  So changing a parameter from `copy` / `view` to `mod` still compiles every
  caller, now mutating the caller's value: the C++ trap the README says Rae
  avoids. Either require `mod` at the call site for a `mod` parameter (the
  spelling `x: mod track` works because arguments use `:`), or correct the
  README. Independent of option N.
- **Cost changes silently with the type.** An `Int` → `Track` refactor keeps
  every meaning, but each `=` of a `Track` holding a `List` becomes a deep
  copy. Noted, not addressed.
