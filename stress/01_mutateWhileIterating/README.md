# 01 — Mutate while iterating

**Verdict: `handles`** — Rae rejects the program at compile time.

## The foot gun

A loop walks a list, and the loop body appends to that same list. Whether the
new element is visited, skipped, or the loop never ends depends on the
language's iterator, and the answer is rarely what the author meant.

```rae
loop let x: view Int in xs {
  if x is 2 {
    xs.add(value: 99)
  }
}
```

## What Rae does

```
Main.rae:11:7: cannot mutate a List while a collection loop borrows its elements
```

The collection loop borrows the list's elements for its whole extent, and a
borrowed list cannot be mutated. No run-time check, no surprise: the program
does not exist.

## Elsewhere

| language | behaviour |
|---|---|
| Python | runs; the appended element is visited too — an append inside a `for` over the same list runs away |
| JavaScript | runs; `for…of` over a growing array never ends |
| Java | `ConcurrentModificationException` thrown at run time on the next iteration |
| Go | runs; `range` copied the slice header before the loop, so the append is silently invisible to it |
| Rust | compile error — `xs` is borrowed immutably by the loop, so it cannot be borrowed mutably |
| C | undefined behaviour — the loop keeps walking memory that `realloc` may have moved |

## Expected outcome

Compile error containing `cannot mutate a List while a collection loop borrows
its elements`. Asserted by `stress/run.sh` through the pack's `stress.expect`
block.
