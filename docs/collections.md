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

## Out-of-range access: reads miss, writes are ignored with a warning

`List` has two answers for an index that does not exist, and they are not the
same answer on purpose:

| call | index out of range |
|---|---|
| `copyAt(index:)`, `viewAt`, `modAt` | return `none` — a read may miss, and the caller says what happens with `if let` |
| `copyAtFallback(index:, fallback:)` | return the fallback |
| `set(index:, value:)`, `insert(index:, value:)`, `remove(index:)`, `swapRemove(index:)` | bounds-checked, **ignored** (the list is unchanged), and reported: `warning: List.set: index 10 is out of range for length 3` on stderr; the program continues |
| `prefetch(index:)` | does nothing (it is only a cache hint: in range it prefetches the element's cache lines for a read soon, so a loop over scattered elements can ask for the one a few iterations ahead; it changes no result) |

A read past the end is a question ("is there something at 10?") and the
optional is the honest reply. A write past the end has no slot to land in,
so it is ignored — but never quietly: `set` used to ignore the write with no
word at all, which stress case `stress/08_silentNoOpWrite` asserted as the
foot gun it was (a bug found by archaeology). Stopping the program instead
(an exception, a panic, a trap) was considered and rejected: a shipped game
or UI that dies on an ignored write is worse for its user than one that
carries on. So: check, ignore, warn — **the same in every build profile**.

The warning is `runtimeWarning(message:)` in `lib/core/Core.rae`; its
sibling `runtimeError(message:)` (one line, exit 70) is for a state the
program cannot continue from, and List does not use it. A write whose index
may be out of range checks `length` first, or `add`s.

**A hot loop that writes a known number of elements** grows the list once
with `addDefaults(count:)` (that many default values: 0, "", a zero-filled
struct; one capacity check, one `memset`) and then writes each element with
`set` at an index it keeps itself. An `add` per element is correct but
slow there: each one's length update must land before the next can be
checked, so the writes run one after another (base64 encoding of 1 MB:
2.4 ms with `add`, 0.9 ms this way).

**Checks in counted loops are paid once.** When a loop counts with
`i < n` / `i + k <= n` and steps its counters by constants, and indexes its
lists (and Strings, through `byteAt`) at `counter + constant`, the compiler
checks the loop's whole index range against the lengths before it starts and
runs a copy without per-element checks when it fits
(`compiler/src/c_loop_versioning.c`). Anything else, including a range that
does not fit, runs the checked loop, with the same answers.

**`Array(T, cap: N)` has the same API and the same answers** — `copyAt` /
`viewAt` / `modAt` return `none`, `copyAtFallback` returns the fallback,
`set` past the cap is ignored with `warning: Array.set: index 20 is out of
range for length 16`, `length()` is the cap, and a collection loop iterates
it in place. There is no `[]` on either collection (docs/array-vs-list-
indexing.md). Array's wrappers are synthesized by the compiler per cap
(`compiler/src/array_methods.c`) rather than written in `lib/core`, because
no Rae signature can range over the cap; they are the same code as List's.
