# 11 — View into a growing list

**Verdict: `handles`** — the call is a compile error. (Flipped from
`footgun`: it compiled, and the call read through a reference into storage
it had just reallocated.)

## The foot gun

A view of one element, handed to a function together with the list as `mod`.
The function grows the list (the backing storage moves), then reads the view.

```rae
if let first: view Int => numbers.viewAt(index: 0) {
  grow(numbers: mod numbers, item: first)
}
```

## What Rae does

```
Main.rae:20:9: cannot mutate a List while a `viewAt`/`modAt` binding aliases one of its elements
    20 |     grow(numbers: mod numbers, item: first)
```

Passing the list as `mod` (spelled `mod numbers`, or a bare `numbers` for a
`mod` parameter) counts as a change to it, like `numbers.add(...)`, while a
`viewAt`/`modAt` binding or a collection loop borrows one of its elements:
the callee may grow it. Before 0.2.4 it compiled and `grow` printed whatever
was at the old address (often still 7, sometimes not). Pass the element by
value, or bind it after the call.

## Elsewhere

Rust rejects the call (a mutable and an immutable borrow at once); C has the
same dangling pointer; Python, JavaScript and Java pass the number by value;
Go keeps the old backing array alive.
