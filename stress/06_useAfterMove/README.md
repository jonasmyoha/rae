# 06 — Use after move

**Verdict: `footgun`** — `own` does not move; the value stays usable with no
diagnostic.

## The foot gun

A function that takes a value over, and a caller that keeps using it. The
languages with moves make this a compile error; the languages without moves
share the container so both sides see every change.

```rae
func consume(xs: own List(Int)) ret Int {
  xs.add(value: 4)
  ret xs.length
}

let n: Int = consume(xs: xs)
log("caller sees {xs.length}, callee returned {n}")
```

## What Rae does today

```
caller sees 3, callee returned 4
```

Exit 0. The callee got a copy: it added to its own list and reported 4, the
caller's list is untouched at 3 and still in scope. `own` promises a transfer
of ownership and delivers a copy, and nothing tells the caller either way.

## Elsewhere

| language | behaviour |
|---|---|
| Python | no moves; the callee mutates the shared list, the caller sees 4 |
| JavaScript | no moves; the callee mutates the shared array, the caller sees 4 |
| Java | no moves; the callee mutates the shared list, the caller sees 4 |
| Go | the callee appends to its own slice header; the caller usually sees 3 — a classic Go surprise |
| Rust | compile error: `use of moved value: xs` |
| C | no moves; whatever the pointer points at is shared |

## Expected outcome (the current bug, asserted)

Exit 0, stdout exactly `caller sees 3, callee returned 4`. When `own` becomes
a real move (a later use is a compile error) this case flips to `handles`
with `outcome: compile-error`.
