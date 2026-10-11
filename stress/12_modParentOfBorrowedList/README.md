# 12 — Mod the parent of a borrowed list

**Verdict: `handles`** — the call is a compile error. (Before 0.2.5 it
compiled, and the read after the call printed 5196 where the list held 7:
memory the list had already freed.)

## The foot gun

A view into a list that lives inside a struct, while the whole struct is
handed to a function as `mod`. The function grows the list, so its storage
moves, and the view is read afterwards.

```rae
if let first: view Int => shelf.numbers.viewAt(index: 0) {
  grow(shelf: mod shelf)
  log("first {first}")
}
```

## What Rae does

```
Main.rae:22:9: cannot mutate a List while a `viewAt`/`modAt` binding aliases one of its elements
```

You cannot `mod` a parent while you hold one of its members: passing (or
assigning) a place that contains the borrowed list counts as changing the
list, exactly like `shelf.numbers.add(...)`. The fix in code is to give the
callee only the part it changes (`grow(numbers: mod shelf.numbers)` is still
refused here, `grow(other: mod shelf.otherList)` is fine), or to move the
list out of the struct for the loop and put it back after.

## Elsewhere

Rust refuses it the same way (a mutable borrow of `shelf` while
`shelf.numbers` is borrowed); C dangles; Python, JavaScript and Java hold
the number by value; Go keeps the old backing array alive.
