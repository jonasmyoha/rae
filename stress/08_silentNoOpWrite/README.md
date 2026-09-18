# 08 — Silent no-op write

**Verdict: `handles`** — the write is bounds-checked and dropped, and the
program says so. (Flipped from `footgun`: it used to be dropped without a
word.)

## The foot gun

Writing to an index the list does not have.

```rae
var numbers: List(Int) = [1, 2, 3]
numbers.set(index: 10, value: 5)
log("length {numbers.length}")
```

## What Rae does

```
warning: List.set: index 10 is out of range for length 3      (stderr)
length 3                                                       (stdout)
```

Exit 0. The rule (`docs/collections.md`): a **read** past the end may miss,
so `copyAt` returns `none` and `copyAtFallback` its fallback; a **write**
past the end is checked, dropped, and reported on stderr — `set`, `insert`,
`remove` and `swapRemove` alike, in every build profile. The program never
stops for it, and the bug is never silent. Stopping the program (an
exception, a panic) was considered and rejected: a shipped game or UI that
dies on a dropped write is worse for its user than one that carries on;
dropping it *quietly* was rejected too, because that hides the bug forever.

The sweep this rule forced found one real caller in the tree:
`lib/compress/Inflate.rae` wrote `offs[16]` on a 16-element list every time
it built a Huffman table — harmless only because that entry was never read.

## Elsewhere

| language | behaviour |
|---|---|
| Python | `IndexError: list assignment index out of range` — the program stops |
| JavaScript | the array grows to 11 with holes — a different foot gun |
| Java | `IndexOutOfBoundsException` — the program stops |
| Go | run-time panic: `index out of range [10] with length 3` |
| Rust | panics: `index out of bounds` |
| C | writes past the buffer — memory corruption |

## Expected outcome

Exit 0, stdout exactly `length 3`, stderr exactly the warning line above.
