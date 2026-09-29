# Binding syntax: how other languages spell init and `=`

Status: **exploration, closed 2026-09-30: option N was chosen** (see
`docs/binding-modes-design.md`). Kept as the record of the alternatives. A
companion to `docs/binding-modes-design.md`.

"The settled design" and option A below mean the `name: mode T: value`
grammar that was settled on 2026-09-29 and withdrawn when N was chosen. The
"cold-read" findings come from a test run against it: an agent with no
history rewrote `examples/121_ui_editor/inspectorSystem/Panels.rae` from that
design document alone and reported every place the rules were unclear or
costly. That document settles the ownership model:
receivers, sources, and the matrix between them. This one only asks how the
declaration should be *spelled*.

The worry: the settled grammar writes a declaration as
`name: mode Type: value`, so every binding carries two colons:

```rae
# new (binding-modes-design.md)
let anchor: copy EntityId: inspectorSystem.selected
var parentLabel: own String: entityLabelOf(
  world: world
  entityId: parentOf(world: world, uiSystems: uiSystems, entityId: anchor)
)
if parentLabel.length() is 0 {
  parentLabel = "(none)"
}
chromeSystem.inspectorParent = own parentLabel

# old (today's compiler)
let anchor: EntityId = inspectorSystem.selected
var parentLabel: String = entityLabelOf(
  world: world
  entityId: parentOf(world: world, uiSystems: uiSystems, entityId: anchor)
)
if parentLabel.length() is 0 {
  parentLabel = "(none)"
}
chromeSystem.inspectorParent = own parentLabel
```

The snippet has four steps:

1. bind a `let` from a field (`anchor`, an `EntityId` read out of the
   inspector's state, so `copy`);
2. declare a `var` from a new value (`parentLabel`);
3. patch the `var`;
4. move it into the panel's state with `= own`, which ends `parentLabel`.

Every language below does the same four steps, with its own spelling of an
immutable and a mutable local.

What pulls the other way: `name: value` is how a Rae call passes an argument
and how a struct literal sets a field. A declaration that ends in
`parentLabel: …value…` reads the same way. The double colon comes from
fusing two shapes Rae already has: the parameter declaration `name: mode T`
and the argument `name: value`.

## 1. The same snippet in other languages

Each version keeps the language's own naming and idiom. The interesting
parts are the declaration, the reassignment, and where the language writes
ownership (if anywhere).

### Rust

```rust
let anchor: EntityId = inspector_system.selected; // EntityId is Copy
let mut parent_label: String = entity_label_of(&world, parent_of(&world, &ui_systems, anchor));
if parent_label.is_empty() {
    parent_label = "(none)".to_string();
}
chrome_system.inspector_parent = parent_label; // moves: parent_label is unusable after
```

- The introducer is `let`, with `mut` for mutability; the type follows `:`
  and the value follows `=`.
- Ownership is split across three places:
  - the **type** (`&T`, `&mut T`),
  - the **expression** (`&x`, `&mut x`),
  - an implicit move for a non-`Copy` type (`let b = a;` ends `a`).
- Copies are explicit calls (`.clone()`); `Copy` types copy silently.
- There are no named arguments, so there is no call-site shape for a
  declaration to mirror.
- Rust also lets a block be the value:
  `let x = { let l = …; if l.is_empty() { … } else { l } };`

### C++

```cpp
const EntityId anchor = inspectorSystem.selected;
std::string parentLabel = entityLabelOf(world, parentOf(world, uiSystems, anchor));
// or direct-list-init: std::string parentLabel{entityLabelOf(...)};
if (parentLabel.empty()) {
    parentLabel = "(none)";
}
chromeSystem.inspectorParent = std::move(parentLabel); // parentLabel stays valid but unspecified
```

- The type comes first, then the name.
- Initialisation has at least four spellings: `= x`, `(x)`, `{x}` and
  `= {x}`, which differ in subtle ways (narrowing, explicit constructors).
  It is the textbook case of what not to multiply.
- Ownership lives in the **type**: `T&`, `const T&` (an alias), `T&&`, and
  smart pointers. A copy is the default for a value type; a move is the
  library call `std::move(x)` on the value side.

### Odin

```odin
anchor: EntityId = inspector_system.selected // no immutable locals: `::` is compile-time only
parent_label: string = entity_label_of(world, parent_of(world, ui_systems, anchor))
// inferred: parent_label := entity_label_of(...)
if len(parent_label) == 0 {
    parent_label = "(none)"
}
chrome_system.inspector_parent = parent_label // copies the header; both point at the same bytes
```

- There is no introducer keyword; the declaration *is* `name : type = value`.
- The syntax is built around the colon. `name := value` infers the type, and
  a **constant** is `name : type : value`, or `name :: value` when inferred.
- **So Odin already has Rae's double colon, and in Odin it means
  "constant".** The second colon is the compile-time binder, `=` the
  run-time one.
- Named arguments use `=`: `f(world = world)`.
- There are no ownership modes; memory is manual, with allocators.

### Jai

```jai
anchor: EntityId = inspector_system.selected; // no immutable locals either
parent_label: string = entity_label_of(world, parent_of(world, ui_systems, anchor));
if parent_label.count == 0 {
    parent_label = "(none)";
}
chrome_system.inspector_parent = parent_label; // copies the header; freeing is your job
```

- The same family as Odin, which borrowed it: `name : type = value`,
  `name := value`, and constants as `name : type : value` / `name :: value`.
- In Jai, procedures and structs are declared with `::` too
  (`foo :: (x: int) { … }`), so "colon-colon means compile-time binding" is
  a uniform rule there.
- Named arguments use `=`.

### Nim

```nim
let anchor: EntityId = inspectorSystem.selected
var parentLabel: string = entityLabelOf(world, parentOf(world, uiSystems, anchor))
if parentLabel.len == 0:
  parentLabel = "(none)"
chromeSystem.inspectorParent = move(parentLabel) # or plain `=`: a last read moves; ensureMove asserts it
```

- `let` / `var` / `const` introducers, then `name: type = value`.
- **Ownership modes are type modifiers on parameters**, very close to Rae's
  `view` / `mod` / `own`:
  - `proc f(x: var T)` is a mutable parameter (`mod`);
  - `sink T` takes ownership (`own`);
  - `lent T` is a borrowed return (`ret view`).
- The mode sits after the parameter's colon, before the type, like Rae's.
  It is not written on locals.
- Named arguments use `=`: `f(world = world)`.

### Scala

```scala
val anchor: EntityId = inspectorSystem.selected
var parentLabel: String = entityLabelOf(world = world, entityId = parentOf(world, uiSystems, anchor))
if parentLabel.isEmpty then
  parentLabel = "(none)"
chromeSystem.inspectorParent = parentLabel // shares the immutable String; the GC owns it
```

- `val` / `var` introducers, then `name: Type = value`.
- **Named arguments are also `name = value`**, so Scala gets the mirroring
  Rae wants, but through `=` rather than `:`. The declaration's tail
  `parentLabel … = value` and the argument `world = world` share one binder.
- Kotlin and Python do the same (`var x: String = …`, `f(world = world)`;
  Python `x: str = …`, `f(world=world)`).

### Ada

```ada
Anchor       : constant Entity_Id := Inspector_System.Selected;
Parent_Label : Unbounded_String :=
  Entity_Label_Of (World => World,
                   Entity_Id => Parent_Of (World, Ui_Systems, Anchor));
...
if Length (Parent_Label) = 0 then
   Parent_Label := To_Unbounded_String ("(none)");
end if;
Chrome_System.Inspector_Parent := Parent_Label;  -- copies: Ada has no move operator
```

- The declaration is `Name : Type := Value`. There is no introducer keyword;
  declarations live in the `declare` / `is` region before `begin`.
- **A mode word after the colon, before the type, is native Ada**, and it
  is the exact shape Rae's design arrived at:
  - `X : constant Integer := 5;`
  - parameters `X : in T`, `X : out T`, `X : in out T`, `X : access T`.
- `:=` is both initialisation and assignment; `=` is equality.
- **Aliases get a keyword instead of an operator:**
  `V : T renames Some.Record.Field;`. A renaming is not an initialisation,
  so it does not use `:=`. That is the same distinction Rae's retired `=>`
  drew.
- Named arguments and aggregates use `=>`: `F (World => World)`,
  `(X => 1, Y => 2)`.

### Swift

```swift
let anchor: EntityId = inspectorSystem.selected
var parentLabel: String = entityLabelOf(world: world, entityId: parentOf(world: world, uiSystems: uiSystems, entityId: anchor))
if parentLabel.isEmpty {
    parentLabel = "(none)"
}
chromeSystem.inspectorParent = consume parentLabel // parentLabel ends; a later use is an error
```

- `let` / `var` introducers, then `name: Type = value`.
- **Swift's calls are Rae's calls** (`f(world: world)`, labels with a
  colon), and its declarations still use `=`. Swift has lived with
  "`:` in calls, `=` in declarations" since 2014; the split is not reported
  as confusing, because `:` after a declared name always introduces a
  *type*, never a value.
- Ownership:
  - parameter modifiers `borrowing` / `consuming` / `inout`, which sit in
    the type position (`func f(x: consuming T)`);
  - value-side operators `consume x` and `copy x`;
  - `inout` arguments are marked `&x` at the call.

### C#

```csharp
EntityId anchor = inspectorSystem.Selected; // no immutable locals (only `ref readonly`)
string parentLabel = EntityLabelOf(world: world, entityId: ParentOf(world, uiSystems, anchor));
if (parentLabel.Length == 0) {
    parentLabel = "(none)";
}
chromeSystem.InspectorParent = parentLabel; // shares the reference
```

- Type-first declaration with `=`.
- **Named arguments use `:`**, like Rae and Swift.
- Alias locals write the mode on both sides: `ref T x = ref y;`, and
  `ref readonly T x = ref y;`. Parameter modes are `ref`, `in`, `out`.

### Go

```go
anchor := inspectorSystem.selected // no immutable locals
parentLabel := entityLabelOf(world, parentOf(world, uiSystems, anchor))
// or: var parentLabel string = entityLabelOf(...)
if len(parentLabel) == 0 {
	parentLabel = "(none)"
}
chromeSystem.inspectorParent = parentLabel // copies the header; strings are immutable
```

- **`:=` declares, `=` assigns.** Two operators, split exactly on "is this
  name new?". The type comes after the name with no colon:
  `var parentLabel string`.
- `:=` always infers the type, which Rae forbids, but the *split* is the
  interesting part.

### Zig

```zig
const anchor: EntityId = inspector_system.selected;
var parent_label: []const u8 = entityLabelOf(world, parentOf(world, ui_systems, anchor));
if (parent_label.len == 0) {
    parent_label = "(none)";
}
chrome_system.inspector_parent = parent_label; // copies the slice; who frees it is a convention
```

- `const` / `var` introducers, then `name: T = value`.
- There are no ownership modes; allocators are explicit.

### Mojo

```mojo
var anchor: EntityId = inspector_system.selected # Mojo dropped `let`
var parent_label: String = entity_label_of(world, parent_of(world, ui_systems, anchor))
if len(parent_label) == 0:
    parent_label = "(none)"
chrome_system.inspector_parent = parent_label^ # transfer: parent_label is uninitialised after
```

- `var name: T = value`.
- Argument conventions go *before* the parameter name:
  - no keyword means an immutable reference;
  - `mut` is mutable;
  - `var` (formerly `owned`) takes ownership;
  - `ref` and `out` also exist.
- A transfer is the postfix sigil `x^` at the call site
  (`take_text(message^)`). Copies are implicit for copyable types.

### Hylo (formerly Val)

```hylo
let anchor: EntityId = inspector_system.selected
var parent_label: String = entity_label_of(world, parent_of(world, ui_systems, anchor))
if parent_label.is_empty() { &parent_label = "(none)" }
&chrome_system.inspector_parent = parent_label // consumes it; a later use needs .copy()
```

- **The binding introducer carries the mode**:
  - `let` is immutable;
  - `var` is mutable;
  - `inout x = &point.x` is a mutable *projection* of a part of an
    existing value (an alias).
- The introducer word does the work Rae gives to the mode word; there is no
  second word next to the type.
- Assignment into a `var` consumes the source. Copies are explicit
  (`.copy()`), and the compiler suggests them.
- Mutation is marked with `&` on the mutated name.

### Austral, Vale, Carbon, Pascal

- **Austral:** `let x: T := e;` and `var x: T := e;` (`:=` with a typed
  name, linear types). Borrows are a block statement:
  `borrow x as ref in R do … end;`. A linear value is consumed by any use,
  so storing `parentLabel` moves it with no marker.
- **Vale:** `x = e;` *declares*; `set x = e;` *mutates*. The keyword goes on
  the rarer operation, reassignment, not on the declaration. An owning
  reference moves when stored: `set chromeSystem.inspectorParent = parentLabel;`.
- **Carbon:** `var x: T = e;` and `let x: T = e;` (a `let` is a value,
  possibly a reference, not a mutable object).
- **Pascal / Delphi:** `var X: T;` in a declaration block, `X := e;` in the
  body. Declaration and initialisation are separate statements. Delphi 10.3
  added the inline `var X: T := e;`. Strings are reference-counted and
  copy-on-write, so `ChromeSystem.InspectorParent := ParentLabel;` shares.

### F# / OCaml

```fsharp
let anchor : EntityId = inspectorSystem.Selected
let mutable parentLabel : string = entityLabelOf world (parentOf world uiSystems anchor)
if parentLabel.Length = 0 then
    parentLabel <- "(none)"
chromeSystem.InspectorParent <- parentLabel // shares the reference
```

- Declaration is `let name : type = value`.
- **Mutation uses a different operator, `<-`**, so `=` in a binding never
  means "overwrite".
- `=` is also equality. That works because a binding is always introduced
  by `let`.

### Lisp family

```lisp
(let* ((anchor (selected inspector-system))
       (parent-label (entity-label-of world (parent-of world ui-systems anchor))))
  (when (zerop (length parent-label))
    (setf parent-label "(none)"))
  (setf (inspector-parent chrome-system) parent-label)) ; shares the object
```

- The binding is a **parenthesised pair** (`(name value)`) inside a `let`
  form that scopes it. Types, where present, are separate declarations
  (`(declare (type string parent-label))`).
- Mutation is a distinct operator (`setf`).

### Summary table

| language | declaration | reassignment | move into a place | named argument | where ownership is written |
|---|---|---|---|---|---|
| **Rae (settled)** | `let` / `var x: own T: v` | `x = v` | `p = own x` | `f(x: v)` | after the name's colon; `own x` on the value side |
| Rae (today) | `let` / `var x: T = v` | `x = v` | `p = own x` | `f(x: v)` | parameters; `=>` for aliases; `own x` |
| Rust | `let mut x: T = v` | `x = v` | `p = x` (implicit) | none | the type (`&`, `&mut`) + the expression (`&x`) |
| C++ | `T x = v` (4 forms) | `x = v` | `p = std::move(x)` | none | the type (`T&`); `std::move(x)` |
| Odin | `x: T = v` | `x = v` | none (header copy) | `f(x = v)` | none (**`x: T: v` is a constant**) |
| Jai | `x: T = v` | `x = v` | none (header copy) | `f(x = v)` | none (**`x: T: v` is a constant**) |
| Nim | `var x: T = v` | `x = v` | `p = move(x)`, or implicit at last read | `f(x = v)` | parameter type (`var T`, `sink T`, `lent T`) |
| Scala / Kotlin | `var x: T = v` | `x = v` | none (GC share) | `f(x = v)` | none |
| Ada | `X : T := V` | `X := V` | none (copies) | `F (X => V)` | after the colon (`constant`, `in out`); `renames` |
| Swift | `var x: T = v` | `x = v` | `p = consume x` | `f(x: v)` | parameter type (`consuming T`); `consume x`, `copy x` |
| C# | `T x = v` | `x = v` | none (GC share) | `f(x: v)` | both sides (`ref T x = ref y`) |
| Go | `x := v` / `var x T = v` | `x = v` | none (header copy) | none | none |
| Mojo | `var x: T = v` | `x = v` | `p = x^` | `f(x=v)` | before the parameter name (`var x: T`); `x^` |
| Hylo | `let` / `var` / `inout x: T = v` | `&x = v` | `&p = x` (consumes) | `f(x: v)` | the introducer (`inout`) |
| Austral | `let x: T := v` | `x := v` | any use (linear) | `f(x => v)` | linear types; `borrow … as` blocks |
| Vale | `x = v` | `set x = v` | `set p = x` (moves) | none | `&x` borrows |
| F# | `let mutable x : T = v` | `x <- v` | none (GC share) | `f(x = v)` (methods) | none |

What the table says:

1. **Nobody uses `:` to bind a local's value.** The universal reading of
   `name: T` is "name has type T". Languages that use `:` in calls (Swift,
   C#, Hylo, Rae) still bind locals with `=` / `:=`.
2. **The languages that mirror call syntax in declarations do it with `=`**
   (Scala, Kotlin, Python, Odin, Jai, Nim): `f(x = v)` and `var x: T = v`.
   Ada mirrors it with `=>` in calls and aggregates, which Rae has
   already retired.
3. **The one existing `name: T: value` form (Odin, Jai) means a constant.**
   A reader with that background will read every Rae `let x: own T: v` as
   a compile-time constant.
4. **Ownership-first languages put the mode either in the type position**
   (Nim, Swift parameters, Ada, Rae) **or on the introducer** (Hylo, Mojo
   parameters). Nobody writes both `let` and a mode word next to the type.
5. **Separating "declare" from "overwrite" is common**:
   - Go's `:=` / `=`;
   - F#'s `=` / `<-`;
   - Vale's `x =` / `set x =`;
   - Hylo marks mutation with `&`.

   Rae currently separates them with `:` / `=`, so it is in good company on
   *that*. What is unusual is the *character* chosen.

## 2. The options for Rae

Every option is shown with the three shapes that matter: the owned local, the
alias local, and a field copy. The parameter stays `name: mode T` in every
option, and so does the move into a place, `chromeSystem.inspectorParent =
own parentLabel`: it is a value position, not a declaration. Option L is the
one where the value side matters more.

```rae
func syncTreeViewport(chromeSystem: view ChromeSystem, world: mod UiWorld) ret own Bool
```

### A. Keep `name: mode T: value` (the settled design)

```rae
var parentLabel: own String: entityLabelOf(world: world, entityId: anchor)
let listView: view ListView: componentView(this: world.listViews, entityId: list)
let members: copy Int: inspectorSystem.selection.length
```

- **For:**
  - it is the literal fusion of the parameter shape and the argument
    shape, so there is one binder character for "a name receives a value";
  - it looks nothing like `=`, so a declaration cannot be mistaken for the
    copying assignment;
  - it is already written in the design.
- **Against:**
  - two colons, three when the value is a struct literal
    (`let active: own Active: { value: … }`);
  - the Odin/Jai "this is a constant" reading;
  - with a struct literal or a named-argument call on the right, the
    binder colon drowns among the field and argument colons.
- The cold-read run flagged `: -` (`var top: own Float: -…`) and
  `if let member: own EntityId: selection.copyAt(…)` as reading badly.

### B. `=` (Swift / Scala / Nim / Carbon / old Rae)

```rae
var parentLabel: own String = entityLabelOf(world: world, entityId: anchor)
let listView: view ListView = componentView(this: world.listViews, entityId: list)
let members: copy Int = inspectorSystem.selection.length
```

- **For:**
  - what every reader already knows;
  - the smallest diff from today's code (only the mode word is added);
  - one colon.
- **Against:**
  - the matrix defines `=` as the value position, which copies unless
    marked. In a declaration the *receiver's mode* decides instead
    (`let c: own Holder = a` moves). So the same `=` would copy in one
    statement and move in the next, the ambiguity the design set out to
    remove;
  - `let v: view T = x` reads as "copy x", which is why `=>` existed.

### C. `:=` (Ada, Pascal, Austral; Go's declare/assign split)

```rae
var parentLabel: own String := entityLabelOf(world: world, entityId: anchor)
let listView: view ListView := componentView(this: world.listViews, entityId: list)
let members: copy Int := inspectorSystem.selection.length
if let member: own EntityId := selection.copyAt(index: k) { … }
loop var i: own Int := 0, i < n, ++i { … }
```

- **For:**
  - one token that still *contains* the colon, so the family resemblance
    to `name: value` survives;
  - visibly different from the copying `=`, which is the Go split:
    `:=` introduces, `=` overwrites;
  - fifty years of precedent (Algol, Pascal, Ada, Smalltalk);
  - the alias case reads acceptably: it is still a binding, not an
    assignment;
  - it does not collide with anything else in Rae.
- **Against:**
  - Go readers associate `:=` with type inference, which Rae forbids. The
    written type right before it makes that misreading short-lived;
  - one more character than `:`.
- **My pick for the mechanical question.** It fixes the double colon
  without touching the model.

### D. A keyword binder

```rae
var parentLabel: own String from entityLabelOf(world: world, entityId: anchor)
let listView: view ListView of componentView(this: world.listViews, entityId: list)
let members: copy Int from inspectorSystem.selection.length
```

- The candidates, and how each reads:
  - `from` reads right for `own` and `copy` ("an own String from …");
  - `of` or `on` read right for aliases;
  - `is` is taken (Rae's equality);
  - `be` (`let x: own T be …`) is quaint;
  - `renames` (Ada) is exact for aliases and odd for values.
- **For:** no punctuation at all; it reads as a sentence.
- **Against:**
  - either one keyword that fits some modes badly, or two keywords and a
    rule about which goes with which;
  - `from` is a likely future keyword elsewhere (imports, ranges);
  - a keyword at the end of a long type line is easy to miss.

### E. The mode is the introducer (Hylo)

```rae
own parentLabel: String := entityLabelOf(world: world, entityId: anchor)
view listView: ListView := componentView(this: world.listViews, entityId: list)
copy members: Int := inspectorSystem.selection.length
```

- **For:**
  - removes the `let own` / `var own` stutter: one word says both "this
    is a new name" and "how it holds its value";
  - reads well: "view listView of type ListView".
- **Against:**
  - mutability now needs its own marker (`own var parentLabel: …`?
    `var own …`?), because `let` / `var` were carrying it. That gives
    `var` a double duty or brings in a fifth introducer;
  - declarations no longer start with the same word, so `grep 'let '`
    stops finding them;
  - parameters say `name: mode T` while locals say `mode name: T`: two
    orders for one idea.

### F. Type-first (C / C# / Java)

```rae
var own String parentLabel: entityLabelOf(world: world, entityId: anchor)
let view ListView listView: componentView(this: world.listViews, entityId: list)
```

- **For:**
  - the tail is *exactly* the call shape, `parentLabel: value`, with one
    colon: the mirroring is achieved literally.
- **Against:**
  - parameters and fields are `name: T`, so locals would be the one place
    with type-first order;
  - long generic types push the name far right
    (`var own List(ListViewRow) widgetRows: …`), and the name is what a
    reader scans for.
- Not recommended, but it is the only option where a declaration *is* a
  named argument.

### G. Brackets around the name and type (a one-parameter header)

```rae
var (parentLabel: own String) := entityLabelOf(world: world, entityId: anchor)
let (listView: view ListView) := componentView(this: world.listViews, entityId: list)
let (entityId: own EntityId, found: own Bool) := findEntity(world: world, nodeId: nodeId)
```

- The parenthesised part is *literally* a parameter list, so a declaration
  reads as "these receivers take the value on the right": the same idea as
  a function header. The binder after it can be `:=`, `=` or `:`.
- **For:**
  - the receiver grammar is shared with functions, not merely similar;
  - it extends naturally to destructuring the `ret a, b` multi-return,
    which today has no binding form of its own.
- **Against:**
  - parentheses on every single declaration are noise for the common case;
  - they could be required only for more than one name, but then there are
    two forms.
- `[own String]` in square brackets was also considered. Rae uses `[a, b]`
  for list literals, and brackets around a type read as "array of" in half
  the languages above.

### H. Constructor shape (C++ direct-init, Swift `String(x)`)

```rae
var parentLabel: own String(entityLabelOf(world: world, entityId: anchor))
```

- **For:** one colon, and the value visibly belongs to the type.
- **Against:**
  - it is indistinguishable from a conversion call;
  - it collides with generics: `own List(ListViewRow)(List.create(…))`.
- Rejected.

### I. Block initialiser

```rae
let parentLabel: own String {
  let label: own String := entityLabelOf(world: world, entityId: anchor)
  if label.length() is 0 {
    ret "(none)"
  }
  ret label
}
```

- The value is a block whose `ret` initialises the name.
- **For:**
  - one form for simple and computed initialisation;
  - it removes this document's own example pattern ("declare `var`, then
    patch it with an `if`"), letting `parentLabel` be a `let`.
- **Against:**
  - `ret` inside a block that is not a function is a new meaning for
    `ret`;
  - it adds a feature rather than choosing a spelling.
- Worth remembering separately; it does not answer the colon question on
  its own, since a one-line value still needs a binder.

### J. Trailing type

```rae
var parentLabel: entityLabelOf(world: world, entityId: anchor) as own String
```

- **For:** the head is exactly `name: value`.
- **Against:**
  - the type and mode arrive after a possibly 5-line call;
  - `as` reads as a cast;
  - a declaration no longer shows its type at the name.
- Rejected.

### K. Sigils

| sigil | example | verdict |
|---|---|---|
| `::` | `let x: own T :: v` | Odin/Jai constant, Haskell type annotation, C++ scope. Reads as "type of". No. |
| `<-` / `←` | `let x: own T <- v` | Reads as "gets" (R, F#'s mutation, Go channels, APL). Plausible, but in F# it is the *mutation* operator, the opposite role. |
| `%` | `let x: own T % v` | Modulo. No. |
| `@` | `let x: own T @ v` | Against the no-`@` preference in AGENTS.md, and reads as "at address". No. |
| `$` | `let x$ [own T] @ v` | Shell or template interpolation, money. No meaning to borrow. |
| `.=` / `:-` | `let x: own T :- v` | No precedent worth having (Prolog's `:-` is "if"). |
| `|` | `let x: own T | v` | "or", or a union type. No. |
| `#` | | Rae's comment character. Impossible. |

None of these beats `:=`, which is the sigil option with a history behind
it.

### L. `let` is a value position (own/copy on the value side)

This is a semantic suggestion, not only a spelling one. The matrix shows that
for a `let` / `var`, `own` and `copy` differ in exactly one row, the owned
name `x` (move or copy). Every other row lands the same way or is decided by
the source kind:

- a field copies;
- a new value is taken.

That is exactly how the **value position** already behaves (`=` copies,
`= own x` moves). So a declaration could *be* a value position with a type:

```rae
var parentLabel: String = entityLabelOf(world: world, entityId: anchor)  # new value: taken
let members: Int = inspectorSystem.selection.length                      # field: copied
let b: Holder = a                                                        # copied, a stays
let c: Holder = own a                                                    # moved, a ends
chromeSystem.inspectorParent = own parentLabel                           # the same rule, no type
let listView: view ListView = componentView(this: world.listViews, entityId: list)
```

- `own` / `copy` are written only on parameters and return types, where
  they are a *contract*, and on the value side where a move is wanted.
  `view` / `mod` stay on locals, because an alias really is a different
  kind of name.
- **For:**
  - `=` means one thing everywhere: a value position;
  - almost all ~10,700 existing declarations are already in this form;
  - it removes the mode noise the cold-read run reported
    (`own Bool: false`, `own Int: 0`, and the arbitrary `own Float` vs
    `copy Float` that depended on the shape of the right-hand side);
  - copies stay visible from the line: an unmarked owned name on the right
    of `=` copies, exactly as in an assignment.
- **Against:**
  - it reverses the settled rule "a bare type in a declaration is an
    error" for owned locals;
  - `let v: view T = x` still reads as a copy to a newcomer. That could be
    answered by giving aliases a different binder:
    `let listView: view ListView := componentView(…)`, or Ada's
    `renames`.
- **The combination I would test next:**
  - `let x: T = v` for owned locals, with the matrix's value-position row;
  - `let v: view T := place` for aliases;
  - modes on parameters and returns as settled.

  It is one colon everywhere, `=` never binds an alias, and the change
  from today's code is the smallest of any option.

### M. `=` binds a value everywhere, named arguments included (Scala, Kotlin, Odin, Nim)

Options A to L all keep `name: value` in calls and struct literals, and ask
how a declaration should look next to it. M turns the question around: keep
the declaration's `=`, and change the **call** to match. This is the Scala,
Kotlin, Python, Odin, Jai and Nim family from the survey. It is the only one
where call syntax and declaration syntax genuinely mirror each other.

The rule becomes one sentence: **`:` says what a name is (its mode and
type); `=` says what it gets (its value).** Every position obeys it:

| position | today | M |
|---|---|---|
| parameter | `rows: own List(ListViewRow)` | unchanged |
| type field | `name: String` | unchanged |
| field default | `min: Int = -1` | unchanged |
| local | `let anchor: EntityId = …` | `let anchor: copy EntityId = …` (or unchanged, M2) |
| named argument | `f(world: world)` | `f(world = world)` |
| struct literal | `{ name: own name, count: 1 }` | `{ name = own name, count = 1 }` |
| assignment | `x = v`, `p = own x` | unchanged |

Rae already has half of this. A field default in a type is written with `=`
today (`lib/ui/Components.rae`: `min: Int = -1`), and so is every local
declaration in the current compiler. The two positions that use `:` for a
value, named arguments and struct-literal fields, are the outliers. The
settled design (option A) adds a third, and it does not say what becomes
of `min: Int = -1`. M makes that question disappear.

The mirroring, spelled out: a call binds the callee's parameters the way a
declaration binds a local, and a struct literal fills fields the way an
assignment does.

```rae
func store(holder: own Holder)       # declares the receiver: name, mode, type
store(holder = a)                    # binds it, as if `let holder: own Holder = a`

type Holder {
  label: String                      # declares the field: name, type
}
let holder: own Holder = { label = name }   # fills it, as if `holder.label = name`
```

#### M1. With `own` / `copy` on locals (the settled model, respelled)

```rae
type ComponentTally {
  name: String
  count: Int = 0
}

type SizeAxis {
  mode: SizeMode
  min: Int = -1
  max: Int = -1
}

func addWidgetRow(
  rows: mod List(ListViewRow)
  id: view String
  name: view String
  value: view String
  indent: view Int
  action: view String
) {
  let row: own ListViewRow = {
    keys = { "id", "name", "value", "indent", "action" }
    values = { id, name, value, "{indent}", action }
  }
  rows.add(value = own row)
}

func labelOf(tally: view ComponentTally) ret copy String {
  ret tally.name
}

func setWidgetRows(world: mod UiWorld, chromeSystem: view ChromeSystem, rows: own List(ListViewRow)) {
  let list: own EntityId = editorListEntityId(
    chromeSystem = chromeSystem
    world = world
    source = EditorListSource.components
  )
  let active: own Active = { value = rows.length > 0 }
  componentSet(this = world.actives, entityId = list, data = active)
  setEditorListRows(world = world, list = list, rows = own rows)
}

# the survey snippet
let anchor: copy EntityId = inspectorSystem.selected
var parentLabel: own String = entityLabelOf(
  world = world
  entityId = parentOf(world = world, uiSystems = uiSystems, entityId = anchor)
)
if parentLabel.length() is 0 {
  parentLabel = "(none)"
}
chromeSystem.inspectorParent = own parentLabel

# the other shapes
let listView: view ListView = componentView(this = world.listViews, entityId = list)
if let member: own EntityId = selection.copyAt(index = k) { … }
loop var i: own Int = 0, i < n, ++i { … }
tallies.add(value = ComponentTally { name = own name, count = 1 })
ret ComponentTally { name = tally.name, count = tally.count + 1 }
```

Here `=` means "the receiver on the left takes this value, as the matrix
says". The receiver is either:

- a declared name, with its mode on the same line (a local) or in the
  callee's signature (an argument); or
- a modeless value position (an assignment, a literal field), which is the
  matrix's last column: copy, or move with `own`.

This answers option B's objection, that `=` would copy in one statement and
move in the next. The operator never decides; the receiver does, and
option A has exactly the same property, spelled with `:`.

#### M2. Without `own` / `copy` on locals (M combined with option L)

Parameters and return types keep their modes, because there they are a
contract. Locals carry a mode only when they are aliases. Types, function
signatures and calls are identical to M1; only the locals change:

```rae
let anchor: EntityId = inspectorSystem.selected         # a field: copied
var parentLabel: String = entityLabelOf(                # a new value: taken
  world = world
  entityId = parentOf(world = world, uiSystems = uiSystems, entityId = anchor)
)
if parentLabel.length() is 0 {
  parentLabel = "(none)"
}
chromeSystem.inspectorParent = own parentLabel          # moved: parentLabel ends

let active: Active = { value = rows.length > 0 }
let b: Holder = a                                       # copied, a stays
let c: Holder = own a                                   # moved, a ends
let listView: view ListView = componentView(this = world.listViews, entityId = list)
if let member: EntityId = selection.copyAt(index = k) { … }
loop var i: Int = 0, i < n, ++i { … }
```

In M2 the whole ownership vocabulary for a local fits in two words:

- `own` on the right of `=` moves;
- `view` / `mod` on the left makes an alias.

Everything else is a copy or a taken new value, readable from the line.

#### For and against

- **For:**
  - one rule for the whole language, `:` for what a name is and `=` for
    what it gets, with no position where a colon introduces a value;
  - real mirroring: `f(world = world)` and `let world: mod UiWorld = …`
    are the same shape, and so are `{ label = name }` and
    `holder.label = name`;
  - no double colon, no `:=`, no new sigil;
  - it keeps the field-default `=` Rae already has, and (in M2) nearly all
    ~10,700 existing declarations;
  - the parser has no ambiguity to resolve. Rae compares with `is`, not
    `==`, and assignment is a statement, not an expression, so `f(a = b)`
    can only be a named argument.
- **Against:**
  - every named argument and struct-literal field in the tree changes.
    That is most lines of Rae, although the rewrite is purely mechanical
    (the formatter's parser already knows each position);
  - `f(a = b)` reads as an assignment inside a call to anyone from C or
    JavaScript. It cannot *be* one in Rae, but the habit is real;
  - Rae loses the Swift / C# / Objective-C call look (`f(world: world)`)
    and gains the Scala / Kotlin / Python one. Neither is more common; it
    is a family choice;
  - `{ name = own name, count = 1 }` looks like a block of assignments.
    The parser knows a literal from its expression position, but this
    closes the door on option I (block initialisers), whose `{ … }` after
    a declaration would be the same shape;
  - `let listView: view ListView = …` binds an alias with the same `=` that
    copies elsewhere. The mode is on the same line, but a reader skimming
    the right-hand side sees a copy. This is the objection option L also
    carries, and the same answers apply (`:=` or `renames` for aliases
    only).
- `.raescene` files are JSON and keep `"name": value`. They are data, not
  Rae source, so that is not a conflict.

### N. Labels take `:`, places take `=`, aliases take `=>` (today's spelling)

N keeps the spelling Rae has today and gives it a principle. It changes no
character in the tree. Every binder in the language has exactly one meaning:

| binder | meaning | where |
|---|---|---|
| `:` | a **label**: a name's type, or a slot in *another* namespace getting a value | parameters, fields, named arguments, struct-literal fields |
| `=` | a **place here is written now**: copy, or move with `own` | locals, assignments, field defaults |
| `=>` | a **name is bound to an existing place**: an alias, no value moves | `let v: view T => place`, `if let v: view T => opt` |

The distinction behind the first two rows is scope. The left side of `f(world:
world)` or `{ name: n }` is not a place the caller can see. It is a parameter
of the callee, or a field of the type being built: a slot defined somewhere
else, filled from here. The left side of `x = v` or `holder.label = v` is a
place that exists in this scope and is written at this moment.

- `:` states a relation, as it does in JSON, CSS, Smalltalk and Swift
  labels, and in every type annotation.
- `=` performs an action.
- `=>` is the third thing, neither a label nor a write. It says "this name
  refers to that place". That is why it deserves its own spelling, the way
  Ada's `renames` does.

Rae's named arguments must follow the parameters' declared order, so they
are labels that confirm position, not independent assignments that could be
reordered. That is the Smalltalk / Swift reading of `:`, not the
Python / Scala reading of `=`.

Semantically N is option L plus the settled matrix:

- locals take their value through the value-position row, so an unmarked
  source copies and `own x` moves;
- `view` / `mod` locals bind with `=>`;
- parameters carry modes as settled, because there they are a contract:
  `own` versus `copy` tells the caller whether its name ends or survives;
- a return type carries a mode only when it returns an alias
  (`ret view T`, `ret mod T`, `ret opt view T`). A plain `ret T` returns a
  value, and `ret x` is a value position like `=`: a field copies, an owned
  local is handed over, a new value is taken.

Why returns drop `own` / `copy`: the caller cannot tell them apart. Either
way it receives a fresh value it owns. The word only says whether the body
may return a field (`copy`) or not (`own`), which describes the body, not a
promise to the caller. It would almost always be `own`: all 8 returns in
`Panels.rae` are, and the withdrawn design's migration table expected the
same for "most functions". Dropping it on returns is the same argument that drops it on
locals.

```rae
type ComponentTally {
  name: String
  count: Int = 0
}

type SizeAxis {
  mode: SizeMode
  min: Int = -1
  max: Int = -1
}

func addWidgetRow(
  rows: mod List(ListViewRow)
  id: view String
  name: view String
  value: view String
  indent: view Int
  action: view String
) {
  let row: ListViewRow = {
    keys: { "id", "name", "value", "indent", "action" }
    values: { id, name, value, "{indent}", action }
  }
  rows.add(value: own row)
}

func labelOf(tally: view ComponentTally) ret String {
  ret tally.name                                         # a field: copied
}

func firstRow(rows: view List(ListViewRow)) ret opt view ListViewRow {
  ret rows.viewAt(index: 0)                              # an alias: the mode stays
}

func setWidgetRows(world: mod UiWorld, chromeSystem: view ChromeSystem, rows: own List(ListViewRow)) {
  let list: EntityId = editorListEntityId(
    chromeSystem: chromeSystem
    world: world
    source: EditorListSource.components
  )
  let active: Active = { value: rows.length > 0 }
  componentSet(this: world.actives, entityId: list, data: active)
  setEditorListRows(world: world, list: list, rows: own rows)
}

# the survey snippet
let anchor: EntityId = inspectorSystem.selected          # a field: copied
var parentLabel: String = entityLabelOf(                 # a new value: taken
  world: world
  entityId: parentOf(world: world, uiSystems: uiSystems, entityId: anchor)
)
if parentLabel.length() is 0 {
  parentLabel = "(none)"
}
chromeSystem.inspectorParent = own parentLabel           # moved: parentLabel ends

# the other shapes
let listView: view ListView => componentView(this: world.listViews, entityId: list)
if let tally: view ComponentTally => tallies.viewAt(index: i) { … }
if let member: EntityId = selection.copyAt(index: k) { … }
loop var i: Int = 0, i < n, ++i { … }
let b: Holder = a                                        # copied, a stays
let c: Holder = own a                                    # moved, a ends
tallies.add(value: ComponentTally { name: own name, count: 1 })
```

This is today's `Panels.rae` spelling, minus the settled design's changes.
Applied to the real `Panels.rae`, N changes exactly one line:
`syncInspectorPanel`'s `world: view UiWorld` becomes `mod`, because the
function writes `world` through `setWidgetRows`. Today's compiler lets a
`view` be passed to a `mod` parameter, and the matrix makes it an error.
The differences from today's *compiler* are all semantic:

- the matrix, owned by semantic analysis;
- mandatory modes on parameters, and on returns that hand out an alias;
- the call-site `own x` as an assertion.

#### N and the README's refactoring argument

`README.md` ("Refactoring stability: change the type, keep the meaning")
argues that in Dart, Java, Kotlin or Swift what `=` does depends on the type.
Promote an `int` to a class, and every `var y = x` silently changes from copy
to alias, in lines the diff never shows. Rae's answer is that **the operator
carries the meaning**: `=` copies whatever the type, aliasing has to be
written `=>`, and a `view` / `mod` alias must agree with the source.

N keeps that argument word for word, and makes it slightly stronger:

- **`=` never aliases.** The README's central claim holds, unchanged.
- **`=` copies whatever the type *and* whatever the source.** Under the
  matrix's value-position row, an unmarked owned name on the right of `=`
  copies too. A move is always the written word `own`, so no refactor can
  turn a copy into a move or back without the line changing.
- **An alias is always `=>`.** Changing a binding from a value to an alias
  cannot happen without the line changing.

Here is how the other options fare against the same claim:

| option | does `=` still carry the meaning alone? |
|---|---|
| **N** | yes: `=` copies (or moves with `own`), `=>` aliases |
| A (settled `: :`) | `=` is only assignment, which copies; declarations bind with `:`, whose effect depends on the declared mode and the source kind |
| C (`:=`) | `=` copies; `:=` binds values and aliases alike, so the mode word decides |
| B, L, M | no: `let v: view T = x` aliases with `=`, so the mode word on the left decides |

In A, C, B, L and M the refactoring property survives in a weaker form. The
meaning is still decided on the line itself (by the mode word), never by the
type, so the Dart failure cannot happen. But the README's sentence "`=`
copies, whatever the type" would have to become "`=` copies unless the left
side says `view` or `mod`". Only N keeps the operator as the carrier.

Two things the README's argument does **not** cover, in any option:

- **Cost changes silently.** The `Int` → `Track` refactor keeps every
  meaning, but a copy of a `Track` holding a `List` is a deep copy. A cheap
  line can become an O(n) line without changing. The meaning is stable; the
  price is not. "Copies are visible from the line" helps, because every `=`
  is a copy by definition, but the size of the copy is decided by the type.
- **The call boundary.** The README says that after the change "every caller
  reads `process(x: mod track)`". But a call-site mode is optional today.
  It appears in one fixture (612) and nowhere in `lib/` or `examples/`, and
  `binding-modes-design.md` says the call site writes nothing. So the C++
  trap the README describes is open in Rae as well: change a parameter from
  `copy Track` to `mod Track`, and every existing `process(x: track)` still
  compiles, now mutating the caller's value. Changing `view` → `own` is
  loud (a later use of the moved name is an error), but `copy` / `view` →
  `mod` is silent. Closing it would mean requiring `mod` at the call site
  for a `mod` parameter. That spelling, `process(x: mod track)`, reads
  naturally only with `:` for arguments (`x = mod track` does not). It is
  a design decision of its own and does not depend on the binder question.

#### For and against

- **For:**
  - zero spelling change, and the smallest migration of any option: only
    the semantic work of the settled design remains;
  - the principle (labels, places, aliases) explains every existing
    binder, instead of choosing a new one;
  - it keeps the README's refactoring argument exactly as written;
  - `:` keeps the Smalltalk / Swift / C# call look, which suits labels that
    must follow the parameters' order;
  - `=` for locals matches the field defaults that already exist
    (`min: Int = -1`).
- **Against:**
  - three binders instead of one or two;
  - `=>` means a lambda in JavaScript and C#, and a match arm in Rust and
    Scala, so a newcomer may misread `let v: view T => x`. It means only
    aliasing in Rae (no other use in `lib/` or `examples/`), and Ada's
    `=>` / `renames` are the closest relatives;
  - the call/declaration mirroring is given up on purpose: a declaration
    is not a named argument, because a local is a place and a parameter
    label is not;
  - `let x: T = v` with no `own` / `copy` reverses the settled rule that a
    bare type in a declaration is an error (the same objection as option L).

## 3. Summary of the choice

There are two independent questions:

1. **Which character binds a declaration's value?**
   - `:` is the fusion, with the double colon and the Odin constant
     reading.
   - `=` is familiar, but in the settled model it collides with the
     copying assignment.
   - `:=` has a long history, keeps the colon family resemblance, and has
     no collision. **I recommend `:=`** if the model stays as settled.
2. **Does a local carry `own` / `copy` at all?** If it does not (option L),
   `=` becomes correct for owned locals, because a declaration is then a
   value position. The binder question shrinks to "what binds an alias",
   where `:=` or `renames` both work.

The mirroring argument, "a declaration looks like `name: value` in a call",
is real but weaker than it seems. In every language surveyed, `:` after a
declared name introduces its **type**. Rae's parameters and fields already
teach that reading (`name: mode T`, `label: String`). A second `:` in the
same line asks the reader to switch meanings mid-line.

Option M gets the mirroring the other way round: the call moves to `=`
instead of the declaration moving to `:`. That is the only option where
calls, literals, declarations, field defaults and assignments all share
one binder, and `:` never introduces a value. Its price is the call-site
rewrite across the whole tree. If mirroring is the goal, **M (as M2)
beats A**. If the goal is the smallest change that removes the double
colon, `:=` still is.

Option N rejects the mirroring goal on principle. A named argument's left
side is a label in the callee's namespace, and a local is a place in this
one, so they *should* be spelled differently. N changes no spelling at all,
and it is the only option that keeps the README's refactoring argument as
written: `=` never aliases, `=>` always does, and a move is always the word
`own`. If the double colon is dropped, **N is the lowest-cost landing**. It
leaves the settled design's semantic work, which is where its value was, and
one open question the binder choice does not touch: whether a `mod`
parameter must be admitted at the call site.

## References

Checked against the live documentation on 2026-09-29: Mojo, Hylo. The rest
are from well-established language references.

- Rust `let` statements:
  https://doc.rust-lang.org/reference/statements.html#let-statements
- C++ initialisation forms: https://en.cppreference.com/w/cpp/language/initialization
- Odin declarations and constants: https://odin-lang.org/docs/overview/#variable-declarations
- Jai (no official public docs): *The Way to Jai*,
  https://github.com/Ivo-Balbaert/The_Way_to_Jai
- Nim `var` / `let`: https://nim-lang.org/docs/manual.html#statements-and-expressions-var-statement
- Nim `sink` / `lent` / `var` parameters: https://nim-lang.org/docs/destructors.html
- Scala variables: https://docs.scala-lang.org/scala3/book/taste-vars-data-types.html
- Scala named arguments: https://docs.scala-lang.org/tour/named-arguments.html
- Ada declarations and parameter modes:
  https://learn.adacore.com/courses/intro-to-ada/chapters/imperative_language.html
- Ada renaming declarations (RM 8.5): http://www.ada-auth.org/standards/12rm/html/RM-8-5.html
- Swift basics: https://docs.swift.org/swift-book/documentation/the-swift-programming-language/thebasics/
- Swift `consume` (SE-0366):
  https://github.com/apple/swift-evolution/blob/main/proposals/0366-move-function.md
- Swift `borrowing` / `consuming` (SE-0377):
  https://github.com/apple/swift-evolution/blob/main/proposals/0377-parameter-ownership-modifiers.md
- C# named arguments:
  https://learn.microsoft.com/en-us/dotnet/csharp/programming-guide/classes-and-structs/named-and-optional-arguments
- Go short variable declarations: https://go.dev/ref/spec#Short_variable_declarations
- Zig variables: https://ziglang.org/documentation/master/#Variables
- Mojo ownership and argument conventions: https://mojolang.org/docs/manual/values/ownership
- Hylo bindings: https://docs.hylo-lang.org/language-tour/bindings
- Austral specification: https://austral-lang.org/spec/spec.html
- Vale guide: https://vale.dev/guide/introduction
- Carbon design overview:
  https://github.com/carbon-language/carbon-lang/blob/trunk/docs/design/README.md
- F# values and `<-`: https://learn.microsoft.com/en-us/dotnet/fsharp/language-reference/values/
- Python variable annotations (PEP 526): https://peps.python.org/pep-0526/
