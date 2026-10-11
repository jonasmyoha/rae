# 11 — View into a growing list

**Verdict: `footgun`** — it compiles, and the call reads through a reference
into storage it has just reallocated.

## The foot gun

A view of one element, handed to a function together with the list as `mod`.
The function grows the list (the backing storage moves), then reads the view.

```rae
if let first: view Int => numbers.viewAt(index: 0) {
  grow(numbers: mod numbers, item: first)
}
```

## What Rae does

It compiles and runs; `grow` prints whatever is at the old address (often
still 7, sometimes not). Rae already refuses `numbers.add(...)` while `first`
is live in the same function, and a collection loop's borrow the same way,
but a `mod` argument of a call was not counted as a change to the list.

## Elsewhere

Rust rejects the call (a mutable and an immutable borrow at once); C has the
same dangling pointer; Python, JavaScript and Java pass the number by value;
Go keeps the old backing array alive.
