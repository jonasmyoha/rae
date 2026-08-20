# match, break/continue, sum types, Result, tuples — investigation + proposal (#420)

Language design is the human's call; this is an investigation and a proposal, not
a decision. It audits what Rae has today, shows where it helps the app
architecture (example 114), and proposes additions informed by Rust/Swift/Odin/Jai.

## TL;DR

- Rae **already has `match`** (statement AND expression) with `case` + `default`,
  and **exhaustive matching on enums is already a hard error** — the headline ask
  ("hard error to not handle a case") is done. No `switch` is needed; `match` is
  the modern spelling.
- Rae **already has** an optional type (`opt T`, `none`, `is not none`, `if let`,
  `case none`) and plain C-style `enum`s.
- Rae **does not have**: `break`/`continue`, enum **payloads** (sum types /
  tagged unions), a `Result` type, or tuples.
- Biggest immediate win needs **no new syntax**: make dispatch tags real `enum`s
  so their `if x is A / if x is B` chains become exhaustive `match` — the compiler
  then forces every site to handle a newly-added variant.

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
- **Exhaustive on enums** — miss a case, get an error; `default` opts out.
- **Expression form** — `match` can produce a value.
- **Optionals matched** — `match get(...) { case none {…} default {…} }`.
- `enum Foo { a b c }` — plain value enums, `Foo.a`, and `if x is Foo.a`.
- Optional type `opt T`; `none`; `is not none` / `is none`; `if let v: T = expr {…}`.

Not present: `switch`, `break`, `continue`, enum payloads, `Result`, tuples.

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
error at every renderer** until each one handles (or `default`s) it. That is
exactly the kind of "can't forget a case" guarantee an engine wants. Same shape
applies to `CameraRigMode` dispatch and the clip/locomotion selection (today
`if clipHit.equals("idle") …`), which want a `ClipKind` enum.

### 2. Systems that carry a "kind" want sum types

An input `InputIntent`, a UI action, a network message, a gameplay event — each is
"one of N shapes, each with its own data." Today that is faked with a `kind: Int`
plus parallel fields (see the tetris3d `e.kind` match, or the `Glb { ok, errorMsg }`
pattern). Real sum types (below) make these exhaustive and payload-safe.

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

## Proposal B — match enhancements (informed by Rust/Swift/Odin/Jai)

Keep the current `match … { case … { } default { } }` shape. Add, in rough priority:

1. **Or-patterns** — `case A, B, C { }` (Rae commas are already optional). Replaces
   `if x is A or x is B …`. Cheap, high-value (the render walk needs it).
2. **Leading-dot member shorthand** — `case .north { }` when the matched type is
   known (Swift/Odin). Cuts the repeated `RenderTag.` noise.
3. **Guards** — `case .north if windy { }` (Swift `where` / Rust `if`). Keeps a
   secondary condition inside the arm instead of a nested `if`.
4. **Int/range patterns** — `case 0 { }`, `case 1..10 { }` (Odin/Rust) for matching
   plain integers with a `default`, so numeric dispatch also gets a single form.
5. **Payload binding** — `case Result.err(msg) { log(msg) }` (needs sum types, C).

Rules to keep/state explicitly:
- **Exhaustive on enums and sum types** (hard error) — already true for enums; extend
  to sum types. `default` is the single opt-out.
- **No implicit fallthrough** (already true) — an arm never falls into the next.
- **Expression form is exhaustive too** — every arm yields the same type; no missing
  path. This is what lets `let x = match …` be safe without a runtime "unreachable".

## Proposal C — sum types (enum payloads / tagged unions)

The larger step, and the enabler for Result and richer matching. Extend `enum` so a
variant may carry fields:

```
enum Shape {
  circle(radius: Float)
  rect(w: Float, h: Float)
  point
}

match s {
  case Shape.circle(r)      { area = 3.14159 * r * r }
  case Shape.rect(w, h)     { area = w * h }
  case Shape.point          { area = 0.0 }
}
```

Design questions to settle (flagging, not deciding):
- **Layout**: tag + max-variant-size union (Rust/Swift/Odin enum). Ownership: a
  variant's payload is `own`ed by the value and dropped by tag when the value drops;
  a `case` binding borrows or moves it per the branch (Rae already reasons about an
  optional's payload this way — `parser.c` around the `if let` drop comment).
- **Naming**: reuse `enum` (Swift/Rust conflate "enum" and "sum type") vs a distinct
  keyword. Reusing `enum` keeps the surface small and is the modern convention.
- **Recursion** (e.g. an AST/tree variant) needs an indirection (`Ptr`/box); can be a
  later phase.

This is the highest-leverage addition but the biggest implementation; it touches
type layout, pattern binding, drop-by-tag, and the backend. Worth staging: (1)
or-patterns + leading-dot on existing enums, (2) payloads, (3) Result on top.

## Proposal D — Result + error handling

Rae already has `opt T` for "value or nothing." For "value or **error**", the code
today returns structs with an `ok: Bool` + `errorMsg`/`error` field (`Glb`,
`Skeleton`, `compileDeferred` status ints). A first-class `Result` built on sum types:

```
enum Result(T, E) { ok(T) err(E) }
```

with `match` forcing both arms handled, is the natural successor. Until sum types
land, the `opt T` + explicit-status-struct convention is fine; do NOT bolt on a
half-Result. Optionally, a `?`-style early-return-on-error operator (Rust `?`, Swift
`try`) is a later ergonomic layer once `Result` exists — propose deferring it.

## Proposal E — tuples

Lowest priority. Rae deliberately does not treat multiline returns as tuples
(`parser.c` note), and multi-value returns are worked around by returning a `List`
(e.g. `meshBounds` returns `List(Float)` of 6 numbers — untyped and error-prone). Two
options: lightweight positional tuples `(Float, Float, Float)` with `let (a, b) = …`
destructuring (Rust/Swift), or just lean on **anonymous/named structs** for grouped
returns (clearer field names, no new type machinery). Recommendation: prefer a small
named struct over tuples unless destructuring ergonomics prove worth a new type; if
added, keep them positional and immutable.

## Recommended order

1. **`enum RenderTag` (+ the other dispatch enums) and convert the `if`-chains to
   `match`.** No language change; immediate "can't-forget-a-pass" safety. Do this as
   part of the 114 systems refactor.
2. **break / continue.** Small, reuses the drop machinery; removes flag-guard hacks.
3. **match or-patterns + leading-dot shorthand.** Small, makes (1) read well.
4. **Sum types (enum payloads)** + payload binding + exhaustiveness. The big one;
   unlocks intents/events/messages as first-class.
5. **Result on sum types** (then optionally a `?` operator). Replaces `ok`-struct
   convention.
6. **Tuples** — only if named structs prove insufficient.
