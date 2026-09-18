# 08 — Silent no-op write

**Verdict: `footgun`** — a write past the end is dropped without a word.

## The foot gun

Writing to an index the list does not have.

```rae
var xs: List(Int) = [1, 2, 3]
xs.set(index: 10, value: 5)
log("length {xs.length}")
```

## What Rae does today

```
length 3
```

Exit 0, stderr empty. `set` did nothing: no error, no growth, the value is
gone. Every language on the table does *something* with the write — fails
loudly, grows the list, or corrupts memory. Silently dropping it is the one
behaviour nobody chose.

## Elsewhere

| language | behaviour |
|---|---|
| Python | `IndexError: list assignment index out of range` |
| JavaScript | the array grows to 11 with holes — a different foot gun |
| Java | `IndexOutOfBoundsException` |
| Go | run-time panic: `index out of range [10] with length 3` |
| Rust | panics: `index out of bounds` |
| C | writes past the buffer — memory corruption |

## Expected outcome (the current bug, asserted)

Exit 0, stdout exactly `length 3`, stderr empty. When `set` traps (or returns
a result the caller must handle) this case flips to `handles`.
