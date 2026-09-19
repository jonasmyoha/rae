# Decision: `Array` and `List` are accessed the same way — no `[]` on either (#649, 2026-09-19)

**Decision: neither `List` nor `Array` has `[]`.** Both are accessed with the
`copyAt` / `viewAt` / `modAt` triad and `if let`, read totally with
`copyAtFallback`, written with `set`, and iterated with a collection loop.

## History

#649 (2026-09) settled that `List` does not get `[]`: a List's length is a
runtime value, so indexed access is inherently fallible, and Rae makes that
explicit through optionals rather than a trap. At that time `Array(T, cap: N)`
kept `arr[i]`, justified by its length being part of the type: a constant
index was checked at compile time, and a dynamic one was bounds-checked at
run time and **aborted** out of range.

That abort was the one place in Rae where an index could stop the program —
and it could not be softened: `arr[i]` returns a bare `T`, and there is no
"zero object" for an arbitrary `T` to hand back instead. So the subscript was
removed (queue, 2026-09-19) and Array was given List's API and List's
behaviour, one behaviour in every build profile:

| | `List(T)` | `Array(T, cap: N)` |
|---|---|---|
| `copyAt(index:)` | `opt T`, `none` out of range | same |
| `viewAt` / `modAt` | `opt view T` / `opt mod T`, aliasing the storage | same |
| `copyAtFallback(index:, fallback:)` | `T` | same |
| `set(index:, value:)` | past the end: ignored + `warning: List.set: …` | past the end: ignored + `warning: Array.set: …` |
| `length` | the runtime length | the cap |
| `loop let x: view T in xs` | yes | yes |

## How Array's methods exist

`Array` is a compiler builtin (a struct wrapping `T[N]`), so no Rae signature
can range over its cap. The compiler therefore synthesizes the wrappers as Rae
source — one small generic module per distinct cap the program uses
(`compiler/src/array_methods.c`) — parsed by the real parser and resolved,
specialized and emitted like `lib/core/List.rae`'s. The bodies index the raw
slot with `this[index]` inside `unsafe`; that subscript is rejected by sema
everywhere else, the way List's buffer intrinsics are unsafe-only.

## Why not keep `[]` on Array with a checked-and-continue behaviour

A subscript that continues past the end must yield *some* `T`, and inventing
one is the "compiler-invented value" #642 rejected. Making it yield `opt T`
would be `copyAt` with a worse spelling, and would still be a different
operator from what `List` uses. One API for both collections is simpler to
read, to generate, and to analyse — the language's goals.
