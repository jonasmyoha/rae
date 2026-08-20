# match, break/continue, sum types, Result, tuples — investigation + proposal (#420)

Language design is the human's call; this is an investigation, not a decision. It
audits what Rae has today, and — deliberately NOT assuming that features other
languages have are desirable — weighs each proposed addition against real Rae code
and Rae's goal of being simpler than Rust. Features that are borrowed abstractions
without a concrete need here are recommended AGAINST.

## TL;DR

- Rae **already has `match`** (statement AND expression) with `case` + `default`,
  and **exhaustive matching on enums is already a hard error** — the headline ask
  ("hard error to not handle a case") is done. No `switch` is needed; `match` is
  the modern spelling.
- Rae **already has** an optional type (`opt T`, `none`, `is not none`, `if let`,
  `case none`) and plain C-style `enum`s.
- **Recommendation after auditing the codebase: do NOT add sum types** (enum
  payloads), **nor a `Result` type, nor range or tuple syntax.** Rae's structs +
  enums + `opt T` + `match` — plus the index-based flat-tree idiom — already handle
  every real "one value, several differently-shaped values" case in the tree,
  **including JSON, the textbook sum type**, which is arguably *cleaner* without
  them (Proposal C). These would add a large implementation and a second mental
  model for marginal gain, against Rae's simplicity goal.
- **Make `default` depend on the subject's type**: **disallow it on enum matches**
  (they must stay exhaustive, so a new variant forces every match to be reviewed) but
  **allow/require it on Int/String matches** (open-ended value spaces — e.g. HTTP
  codes, commands, file tags). `default` is used only **once** in the whole tree (an
  optional match in `lib/core.rae`), so the cost is ~nil (see below).
- **Worth adding** (both need little): make dispatch tags real `enum`s so their
  `if x is A / if x is B` chains become exhaustive `match` (no new syntax — the
  compiler then forces every site to handle a new variant); and `break`/`continue`.

## What Rae has today (audited)

`match` — `compiler/src/parser.c` (`parse_match_statement`, `parse_match_expression`),
`compiler/src/sema.c:1582`. Exhaustiveness at `sema.c:1620`:
`"non-exhaustive match on enum '…': missing case '….…' (add it or use a 'default' arm)"`.

```
match d {
  case Direction.north { ret "n" }
  case Direction.south { ret "s" }
  case Direction.east  { ret "e" }
  case Direction.west  { ret "w" }
}          # remove a case -> compile error naming the missing one
```

Properties that are already right (and match modern languages, not C):
- **No fallthrough** — arms are independent; no `break` needed between cases.
- **Exhaustive on enums** — miss a case, get an error. A `default` arm currently opts
  out on enums too (recommended to DISALLOW on enums — see "`default`" below).
- **Expression form** — `match` can produce a value.
- `enum Foo { a b c }` — plain value enums, `Foo.a`, and `if x is Foo.a`.
- Optional type `opt T`; `none`; `is not none` / `is none`; `if let v: T = expr {…}`.

Not present: `switch`, `break`, `continue`, enum payloads, `Result`, tuples.

## `default`: the subject's type decides (enum = no default; Int/String = default)

The right rule is not "remove `default` everywhere" but a distinction driven by whether
the value space is **closed** (finite, known set) or **open** (effectively infinite):

- **Enum match → exhaustive; `default` is NOT allowed.** Every variant must be named
  (or grouped with an or-pattern). A `default` on an enum is a compile error. This is
  what keeps the guarantee intact: adding a variant forces every relevant `match` to be
  reconsidered — a `default` would silently swallow it, which is the whole thing we're
  avoiding.
- **Int / String match → `default` allowed and REQUIRED**, because the space is
  effectively infinite so no set of `case`s is exhaustive:

  ```
  match code {
    case 200 { … }
    case 404 { … }
    default  { … }
  }
  ```

  String matching is genuinely useful — HTTP-style codes, commands, file-format tags,
  identifiers — so `match` should serve it, but with `default` mandatory (a String/Int
  match with no `default` is a compile error: "add a default arm").

Treat these as **two different match semantics selected by the subject's type**, not
one uniform rule — that is what lets enums stay strict while open-ended values stay
usable. (Bool is a closed set like an enum: `case true`/`case false`, no `default`.)

**Compiler changes this implies:** (1) `default` on an enum subject → error; (2) an
Int/String subject with no `default` → error. Today `default` is allowed on enums and
is the single opt-out; this flips that for enums and requires it for open domains.

**Migration cost is essentially nil.** Audited: `default` appears exactly **once** in
the whole tree — `lib/core.rae`'s `StringMap.has` — and it matches an **optional**, not
an enum (`match get(...) { case none { ret false } default { ret true } }`). Optionals
are a closed 2-state set best handled by `if let` / `is not none` anyway (and without
payload-binding, rejected in Proposal C, a `match` on `opt T` can't bind the value), so
that one site rewrites out of `match` entirely:

```
func has(...) ret Bool { ret get(...) is not none }
```

Every enum `match` in the tree already lists all its cases, so nothing else changes.

**"Handle several enum variants the same way"** — use **or-patterns** (`case A, B, C`,
Proposal B), which name every variant so a new one still forces review; this replaces
the *legitimate* grouping use a `default` would otherwise serve on an enum. For an
"impossible here" variant, list it (or group into one explicit arm).

For enums this is intentionally **stricter than Rust (`_` wildcard) / Swift (`default`)**
— the wildcard is exactly what erodes the guarantee — while Int/String matching lands
right where those languages already put it. No new syntax is needed.

## Where this helps the app architecture (example 114)

### 1. Dispatch tags should be enums → exhaustive match (no new syntax, real safety)

The deferred render-graph walk (`examples/114_walker_character/app.rae`) is an
`if`-chain because the pass tags are plain `Int` consts
(`lib/renderer_deferred.rae`: `const tagGbuffer: Int = 10`, …):

```
if tag is tagShadowD { … }
if tag is tagGbuffer { … }
if tag is tagSsaoD or tag is tagDepthPyramid or tag is tagLighting
  or tag is tagTaaD or tag is tagComposite { … }
if tag is tagUiOverlayD { … }
if tag is tagPresentD { … }
```

Make the tags an `enum RenderTag` and it becomes:

```
match tag {
  case RenderTag.shadow { … }
  case RenderTag.gbuffer { … }
  case RenderTag.ssao, RenderTag.depthPyramid, RenderTag.lighting,
       RenderTag.taa, RenderTag.composite { … }   # (needs or-patterns, below)
  case RenderTag.uiOverlay { … }
  case RenderTag.present { … }
}
```

The payoff is not prettiness — it is that **adding a new pass becomes a compile
error at every renderer** until each one handles it. That is
exactly the kind of "can't forget a case" guarantee an engine wants. Same shape
applies to `CameraRigMode` dispatch and the clip/locomotion selection (today
`if clipHit.equals("idle") …`), which want a `ClipKind` enum.

### 2. Systems that carry a "kind" — already handled by struct + enum + match

An input intent, a UI action, a gameplay event, a JSON value — "one value that is
one of N kinds." Rae models these today with a `kind` **enum** field on a struct and
a `match kind`. In the cases that exist, the per-kind DATA is either the same shape
(`93_raylib_3d`'s `Entity`: `kind` picks cube vs sphere drawing, but pos/size/hue are
shared) or a small "fat struct" where a few fields belong to specific kinds
(`lib/sky.rae`'s `Sky` documents "the `procedural` payload … Ignored by every other
kind"). Both read clearly and cost a few small unused fields. This is NOT a gap that
needs sum types — see Proposal C, which finds even JSON does better without them.

## Proposal A — break / continue (with ownership-drop semantics)

The concern ("they have memory drop effects") is real but already solved elsewhere
in Rae: an early `ret` inside a function must drop the owned (`own`) locals in
scope before it leaves. `break`/`continue` are the **same class of non-local exit**
and reuse the same drop machinery (`compiler/src/ownership.c`):

- `break` — drop every `own` value in the scopes between the `break` and the loop,
  in reverse construction order, then exit the loop.
- `continue` — drop the `own` values created in the current iteration up to the
  `continue`, then jump to the loop's next-iteration point.

Because Rae already inserts scope-exit drops (and handles early `ret`), break/continue
add exit points to that existing analysis rather than a new mechanism. Determinism
is preserved (reverse-construction drop order, identical to normal scope exit).
Recommended: add both; they remove the ugly `var done` / flag-and-guard patterns
that a loop-only language forces. Keyword spelling `break` / `continue` (Rust/Swift/
Odin/C — the least surprising).

## Proposal B — one small match enhancement that follows from Rae

Keep the current `match … { case … { } … }` shape (enum matches exhaustive with no
`default`; Int/String matches take a `default` — see
above). The one addition that
follows naturally from Rae's existing syntax:

- **Or-patterns** — `case A, B, C { }`. Rae's commas are already optional and `is A
  or is B` already exists, so grouping variants in one arm is a natural extension,
  not a borrowed abstraction. It's what turns the render-tag `if x is A or x is B …`
  into one `match` arm. Cheap, high-value.

**Considered and NOT recommended** (borrowed, without a concrete need in Rae code):

- **Guards** (`case … if cond`) — a convenience over a plain `if` inside the arm; no
  case in the tree needs it. Reuses `if`, so it *could* be added later if a real need
  appears, but don't add it speculatively.
- **Range patterns** (`case 1..10`) — Rae has **no `..` range operator** anywhere;
  this is pure Rust/Odin import. Matching a single integer literal (`case 5`) already
  fits the model; ranges do not, and are not needed.
- **Payload binding** (`case Foo.bar(x)`) — depends on sum types, which are
  recommended against (Proposal C).
- **Leading-dot shorthand** (`case .north`) — rejected earlier: Rae has no type
  inference (the shorthand needs it) and a bare `.north` is not found by
  `grep RenderTag.north`. Cases stay fully qualified: `case RenderTag.shadow { }`.

Rules to keep/state explicitly:
- **Exhaustive on enums** (hard error) — already true, and recommended to make
  unconditional by disallowing `default` on enum subjects (Int/String matches keep it —
  see "`default`: the subject's type decides").
- **No implicit fallthrough** (already true) — an arm never falls into the next.
- **Expression form is exhaustive too** — every arm yields the same type; no missing
  path. This is what lets `let x = match …` be safe without a runtime "unreachable".

## Proposal C — sum types (enum payloads): RECOMMEND AGAINST

A sum type lets one value be one of several variants each with its own fields
(`enum Event { damage(amount: Int)  move(x: Int, y: Int)  death }`). Rust/Swift/Odin
have them. The question is not whether they're a nice abstraction in the abstract —
it's whether **actual Rae code** has a "one value, several differently-shaped values"
need that structs + enums + `opt T` + `match` handle poorly. Auditing the tree, the
answer is no.

**The candidates, and how Rae already solves them:**

- **JSON — the textbook sum type** (`lib/json.rae`). A JSON value is genuinely one of
  null / bool / number / string / array / object, each shaped differently. Rae models
  it as a flat `JsonValue { kind: JsonKind, asBool, asNumber, asString, rangeStart,
  rangeLen }` inside a `JsonDoc { values: List(JsonValue), children: List(Int),
  fields: List(JsonField) }`, where array/object children are **ranges into side
  lists by index**. This is not a workaround — it is a *better* representation than a
  recursive sum type: no per-node allocation, cache-friendly, and it sidesteps the
  recursion problem entirely (a recursive sum type needs boxing / a `Ptr` indirection
  Rae would have to grow). If the hardest, most canonical sum-type case comes out
  cleaner without them, that is strong evidence against.

- **`Sky`** (`lib/sky.rae`) — a "fat struct" with `kind: SkyKind` and a superset of
  fields, its own comments labelling which belong to which kind ("the `procedural`
  payload"; "Ignored by every other kind"). Cost: a handful of small unused fields per
  value (a few floats/vec3) and no compiler check that you don't read `turbidity` on a
  `stylised` sky. In practice the code reads the right fields under `match kind`. Clear
  and adequate.

- **`Entity`** (`93_raylib_3d`), UI component `kind`s, render pass tags — same shape
  across kinds; the `kind` dispatches *behavior*, not data. Plain enum + `match` is
  already the perfect fit; a payload would add nothing.

**What sum types would buy, and why it's marginal here:** compiler-enforced payload
access (the code doesn't have this bug class); recursion without manual indexing (the
index approach is often better and needs no boxing); tighter memory (the fat structs
are small). **What they'd cost:** a large implementation (variant layout, payload
binding, drop-by-tag, boxing for recursion, backend work) and a second, heavier mental
model for every reader — the exact kind of accumulated abstraction Rae exists to avoid.

**Recommendation: do not add sum types.** Keep the `kind`-enum + `match` idiom, and the
index-based flat-tree idiom for recursive data. Revisit only if a future subsystem
presents a real recursive, heterogeneous case that the flat/index approach genuinely
cannot express cleanly — and judge it then, on that code, not by analogy to Rust.

## Proposal D — Result: RECOMMEND AGAINST (keep the `ok`-struct convention)

Rae has `opt T` for "value or nothing." For "value or **error**", the code today
returns a struct with an `ok: Bool` field (plus the data, and sometimes an
`errorMsg`/`errorPos`) — `Glb`, `Skeleton`, `JsonDoc`, `scene3d_file`, `hot_reload`,
and more all use it. It is simple, explicit, and reads without any special construct:
`if not g.ok { … }`. A first-class `Result(T, E)` would be built on sum types (rejected
in Proposal C), so it inherits that whole cost, and a `?`/`try` early-return operator
would add hidden control flow on top. The concrete benefit over the `ok`-struct is
small. **Recommendation: keep the `ok: Bool` + data convention**; do not add `Result`
or a `?` operator.

## Proposal E — tuples: RECOMMEND AGAINST (use named structs)

Rae deliberately does not treat multiline returns as tuples (`parser.c` note). The one
real wart is untyped multi-returns worked around with a `List` (`meshBounds` returns
`List(Float)` of 6 numbers — positional and easy to index wrong). But the fix for that
is a **small named struct** (`type Bounds { minX, minY, minZ, maxX, maxY, maxZ: Float }`),
which is clearer than a positional tuple and needs no new type machinery, destructuring
syntax, or `(a, b)` patterns. **Recommendation: do not add tuples**; prefer a named
struct for any grouped/multi return.

## Recommended order

Recommended, and none is a borrowed abstraction:

1. **`enum RenderTag` (+ the other dispatch enums) and convert the `if`-chains to
   `match`.** No language change at all — just use the enums + exhaustive `match` Rae
   already has, for immediate "can't-forget-a-pass" safety. Do this as part of the 114
   systems refactor.
2. **Disallow `default` on enum matches** (keep/require it on Int/String matches) —
   make enum exhaustiveness unconditional. One-line migration (the single optional-match
   site → `is not none`). Pairs with (1): the guarantee only holds without a catch-all
   on enums.
3. **break / continue.** Small; reuses the existing early-`ret` ownership-drop
   machinery; removes the `var done` flag-guard patterns a loop-only language forces.

Optional, only if a concrete need appears later: match **or-patterns** (natural
extension) — see Proposal B.

Explicitly **not** recommended (borrowed abstractions without a concrete need in Rae
code): **sum types** (C), **Result** (D), **tuples** (E), and match **guards / range
patterns / leading-dot shorthand** (B).
