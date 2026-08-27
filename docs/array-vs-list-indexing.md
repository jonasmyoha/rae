# Decision: `Array[i]` vs `List` indexing — the asymmetry is intentional (#649)

**Decision: `List` does NOT get `[]`.** `Array(T, cap: N)` is indexed with
`arr[i]`; `List(T)` is accessed with the `copyAt` / `viewAt` / `modAt` triad and
`if let`. The two spellings reflect two genuinely different abstractions — this
is not a missing-sugar inconsistency to paper over.

## What each does today (verified)

`Array(T, cap: N)` — fixed size, size is part of the **type**:
- `arr[i]` is an **lvalue**. `let c: T = arr[i]` reads a **copy**; `arr[i] = v`
  and `arr[i].field = v` write **in place**; `view`/`mod` bind via `=>`.
- Returns `T` directly — **not** optional.
- **Constant** index out of range is a **compile error**
  (`index 7 is out of bounds for Array(cap: 3); valid indices are 0..2`).
- (A **dynamic** index is currently unchecked — Array's own gap, tracked as
  #655; the docs claim a debug-mode check that isn't implemented. That is an
  Array-internal consistency bug, not a List-vs-Array question.)

`List(T)` — dynamic runtime length:
- `list[i]` is **rejected** by sema with:
  *"a List is not indexed with '[]'; use '.copyAt(index: i)', '.viewAt(index: i)',
  or '.modAt(index: i)' and handle the optional result. '[]' is for
  Array(T, cap: N), whose length is part of its type."*
- The accessors return `opt T` / `opt view T` / `opt mod T`, consumed with
  `if let` (#642/#643).

## Why they differ (and why `[]` on List is the wrong move)

The size of an `Array` is in its type, so indexing is statically bounded and
`arr[i] -> T` reads as infallible (constant OOB is caught at compile time). A
`List`'s length is a runtime value, so indexed access is **inherently fallible**;
Rae makes that fallibility explicit and unavoidable — the accessors return an
optional you must `if let`.

Giving `List` a `[]` operator would force one of three bad choices, none of which
actually delivers "consistency":

1. **`list[i]` returns `opt T`.** Now `[]` yields an optional you must `if let`
   unwrap — clumsier than just writing `copyAt`, and *inconsistent anyway* with
   `arr[i] -> T`. Consistency is not achieved; a worse spelling is.
2. **`list[i]` returns `T`, trapping on OOB.** Rae has **no** trap/panic
   primitive — the #642 decision deliberately rejected adding one ("the else
   branch returns a value the programmer wrote, never a compiler-invented one").
   So this option does not exist without reversing that decision.
3. **`list[i]` returns `T`, unchecked.** A silent out-of-bounds use-after-read/
   write — exactly the footgun class #642/#643/#645 worked to eliminate.

There is a further reason: the `copyAt` / `viewAt` / `modAt` triad (#643) also
names **copy-vs-alias intent** at every call site, which `[]` cannot express.
Collapsing List access to `[]` would lose that distinction as well.

The #642 decision record already lists "no `[]`" among the rejected features;
this note records the specific List-vs-Array rationale.

## Outcome

- `List` keeps the `copyAt`/`viewAt`/`modAt` + `if let` triad; `[]` stays an
  `Array`-only operator. The existing List-`[]` diagnostic already teaches the
  correct alternative, so no message change is needed.
- **No follow-up implementation task** (the follow-up in #649 was conditional on
  "if `List` gets `[]`" — it does not).
- Related, already filed, and *separate* from this decision: #655 (make Array's
  dynamic-index bounds behavior match its docs) and #654 (Array `mod`/`view`
  element binding copies instead of aliasing).
