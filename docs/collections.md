# Collections (`lib/collections/`)

Rae draws a deliberate line between the **core** package and the **collections**
package.

- **core** is what the *language itself* understands. `List(T)` lives here
  (`lib/core/List.rae`) because the compiler knows it by name: collection loops
  require a `List(T)`, `ComponentTable` and the ECS queries are built on it, and
  it is the type an array literal produces. It is auto-loaded and auto-opened —
  always available, bare, no import.
- **collections** is a library of data structures written in ordinary Rae over
  the core primitives (`List`, `Buffer`). Nothing in the compiler knows their
  names. They are things you *ask for*.

That difference is the whole rule of thumb:

> core = the types the compiler itself understands; collections = pure-Rae
> libraries built over `List` / `Buffer`.

## What is in `collections/`

| Module | Type | Notes |
|---|---|---|
| `collections/StringMap` | `StringMap(V)` | open-addressing, `String` keys |
| `collections/IntMap`    | `IntMap(V)`    | open-addressing, `Int` keys |

More will land here as they are written — `Set`, `Queue`, `RingBuffer`,
`SparseSet` and friends — without any compiler change, because a collection is
just Rae code over `Buffer`.

## Using a collection

A collection is **not** in the prelude, so you name it explicitly. Use `open` to
call its constructors and helpers bare (the maps are written for bare/UFCS use):

```rae
open collections/StringMap
open collections/IntMap

func main() {
  var scores: StringMap(Int) = createStringMap(Int, cap: 16)
  scores.set(k: "alice", value: 10)
  if let v: Int = scores.get(k: "alice") { log("alice={v}") }
}
```

`import collections/StringMap` also works if you prefer the qualified form
(`StringMap.createStringMap(...)`); `open` implies `import`.

## Why maps are not in core

They pass none of the "core" tests: the compiler has no built-in knowledge of
`StringMap`/`IntMap` (the C backend only references the type *name* for the
generated drop/`Any` glue, which is true of any user struct), only a handful of
files use them, and they are the first of a family of containers that will grow.
Keeping them in the prelude would auto-load a map into every compile unit that
never asked for one. `List` earns its prelude seat; a hash map does not.

## Out-of-range access: reads miss, writes trap

`List` has two answers for an index that does not exist, and they are not the
same answer on purpose:

| call | index out of range |
|---|---|
| `copyAt(index:)`, `viewAt`, `modAt` | return `none` — a read may miss, and the caller says what happens with `if let` |
| `copyAtFallback(index:, fallback:)` | return the fallback |
| `set(index:, value:)`, `insert(index:, value:)`, `remove(index:)`, `swapRemove(index:)` | **runtime error**: `runtime error: List.set: index 10 is out of range for length 3`, exit code 70 |

A read past the end is a question ("is there something at 10?") and the
optional is the honest reply. A *write* past the end cannot be meant: there
is no slot for it, so there is nothing the program can have wanted except to
be told. `set` used to drop the write silently — no error, no growth, exit 0
— which stress case `stress/08_silentNoOpWrite` asserted as the foot gun it
was; the same rule applied to `insert`/`remove`, which returned quietly. A
Bool result was considered and rejected: nothing forces a caller to look at
it, so it is a silent drop with extra steps.

The trap is `runtimeError(message:)` in `lib/core/Core.rae`, the library's
counterpart to the compiler's own Int traps (`docs/integer-semantics.md`):
one line on stderr, exit 70, in every profile. A write whose index may be out
of range checks `length` first, or `add`s.
