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
