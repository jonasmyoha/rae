# 08 — Silent no-op write

**Verdict: `handles`** — a write past the end is a runtime error that names
the index and the length. (Flipped from `footgun`: it used to be dropped
without a word, exit 0.)

## The foot gun

Writing to an index the list does not have.

```rae
var xs: List(Int) = [1, 2, 3]
xs.set(index: 10, value: 5)
log("length {xs.length}")
```

## What Rae does

```
runtime error: List.set: index 10 is out of range for length 3
```

Exit code 70 from the program (`rae run` reports 1); the `log` line is never
reached. The rule (`docs/collections.md`): a **read** past the end may miss,
so `copyAt` returns `none` and `copyAtFallback` returns the fallback; a
**write** past the end cannot be meant — there is no slot for it — so `set`,
`insert`, `remove` and `swapRemove` trap through `runtimeError`, the
library's counterpart to the compiler's Int traps. A Bool result was
rejected: nothing forces a caller to look at it.

The sweep this rule forced found one real caller in the tree:
`lib/compress/Inflate.rae` wrote `offs[16]` on a 16-element list every time
it built a Huffman table — harmless only because that entry was never read.

## Elsewhere

| language | behaviour |
|---|---|
| Python | `IndexError: list assignment index out of range` |
| JavaScript | the array grows to 11 with holes — a different foot gun |
| Java | `IndexOutOfBoundsException` |
| Go | run-time panic: `index out of range [10] with length 3` |
| Rust | panics: `index out of bounds` |
| C | writes past the buffer — memory corruption |

## Expected outcome

Exit 1 (via `rae run`), stdout empty, stderr exactly the trap line above.
