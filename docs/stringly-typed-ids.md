# Should `sceneId` stop being a `String`? (#78790621)

**Answer: no — keep `sceneId: String`.** Not because typed ids are a bad idea in
general, but because the two things that make them pay elsewhere are already
paid for in Rae by other means, and the one place a wrapper would cost is the
place it would cost most. Investigated 2026-09-23; nothing implemented.

The question named three shapes: a typedef to `String`, a composite type, or
"how is `EntityId` done? maybe same way". Each is answered below.

## 1. A typedef/alias buys literally nothing

**Rae has no type alias**, and the design sketched for one
(`docs/value-types-and-nested-vectors.md` §7) is explicitly that `alias Size =
Vec2` resolves to *Vec2's own `TypeInfo`* — "same `TypeInfo`, same mangled C
type, fully interchangeable". That is the right design for an alias, and it
means an alias gives **zero** type safety: `alias SceneId = String` would leave
`sceneId` and `nodeId` mutually assignable exactly as today. It would be a
comment with a compiler in front of it.

So the typedef option is not a weaker version of the composite option. It is
not a version of it at all.

## 2. `EntityId` is not the precedent it looks like

`EntityId` is a struct, so the surface reading is "do that". But look at what it
holds:

```rae
type EntityId {
  index: Int
  generation: Int
}
```

`EntityId` is not a `String` that was given a name. It is a **generational
handle**: `index` addresses a slot, `generation` distinguishes a live handle
from a stale one pointing at a recycled slot (#703). It is a struct because it
genuinely has **two fields** — the whole point is that the generation travels
with the index. It is POD, so copying is free.

A `sceneId` has no second field. It is a name, and it has to stay a name:
authored `.raescene` data references scenes by string (`"sceneId":
"chrome/Card"`), and every diagnostic prints it. Wrapping it reproduces none of
what makes `EntityId` a type — it only adds a `.value`.

The honest `EntityId`-shaped version of this idea is different: **intern scene
ids into the registry and pass an index**, the way `sceneRegistryIndex(reg:,
key:)` already computes one. That is a real design, but it loses the name in
diagnostics, cannot appear in authored data, and is only valid after
registration — a much larger change than the question is asking about.

## 3. Named arguments already prevent the bug newtypes prevent

The classic case for a newtype is a call like `decodeField(doc, val, key, theme,
diagnostics, sceneId, nodeId)`, where transposing the last two compiles fine and
misreports every diagnostic. **Rae writes the parameter name at every call
site**, so that call is:

```rae
decodeField(… sceneId: sceneId, nodeId: nodeId)
```

A transposition has to be spelled `sceneId: nodeId` — visibly wrong at the exact
place a reader looks. Rae's named arguments are not a style preference here;
they are the mitigation, and they are already mandatory. What a newtype would
still catch is assigning one to the other or returning the wrong one, which is a
much thinner slice of the risk.

## 4. The id↔path boundary is two functions, both already named for it

The one genuinely confusable pair is a scene id versus a scene *file path*, both
`String`. But the conversions are a narrow, documented boundary:

- `sceneIdentityFromFilePath(path:) ret String` (`lib/ui/SceneFile.rae`)
- `scenePackageFile(root:, sceneId:) ret String` (`lib/ui/SceneImports.rae`),
  commented as "the one place a scene reference becomes a file name"

Two functions, each named for the direction it goes. A wrapper across ~265
`sceneId` occurrences to guard a two-function boundary is not a good trade.

## 5. `sceneId` is one of four peers, so it cannot be done alone

Occurrences in `lib/` + `examples/`:

| id | occurrences |
|---|---|
| `nodeId` | 585 |
| `styleId` | 366 |
| `sceneId` | 265 |
| `pageId` | 164 |

Typing `sceneId` and leaving the other three as `String` is *worse* than typing
none: a reader who meets `SceneId` reasonably infers that a bare `String` id is
deliberately untyped, which would be false three times out of four. The real
scope of this question is ~1,380 occurrences across the whole family, not 265.

## 6. The component boundary is where it would actually hurt

`decodeField` (`lib/ui/RegistryGeneric.rae`) is a hand-written chain of
`typeName(fallback) is "…"` arms — `Int`, `Float`, `Bool`, `String`,
`RgbaColor`, `Insets`, `Vec2`, `SizeAxis`, `List(Int)`, `List(String)` — and it
ends with a bare `ret fallback`.

**An unknown field type is silently kept at its default, with no diagnostic.**
So the day `SceneInstance.sceneId` becomes `SceneId`, every authored scene in
the tree decodes it as empty and nothing says so; the failure surfaces later as
a sub-scene that does not mount. Making it work means a new arm in the decoder,
a matching arm in the encoder, and keeping the verbatim save round-trip green
(fixture 884) — for every one of the four id types, in both directions.

`lib/ui/Components.rae` also records that struct-shaped data in a component
table has already caused trouble: the ListView row model is kept as parallel
`List(String)`s rather than a list of `{key, value}` structs, because that is
"the shape the C backend reads back from a component table without the
struct-by-list-index corruption".

## What would change the answer

This is a judgement about today's codebase, not a principle. Revisit if:

- **A real bug is traced to an id mix-up.** None found; the hypothesis is
  currently unsupported by evidence.
- **Rae gains a zero-cost distinct-newtype** (a `type SceneId = String` that is
  nominally distinct but still a `String` at the ABI, with no `.value`). That
  removes the ergonomic cost entirely and reduces this to the decoder work.
  Note this is a *different* feature from the alias in §1, which is deliberately
  interchangeable.
- **Scene ids get interned** for a performance reason. Then the handle exists
  anyway and typing it is free.

Until then `sceneId: String` is the right shape, and `BuiltinScene.sceneId`
(#97288356) should be a plain `String` too.
