# 06 — Use after move

**Verdict: `handles`** — the later use is a compile error. (Flipped from
`footgun`: it used to compile, and the caller read a value the callee had
already taken over.)

## The foot gun

A function that takes a value over, and a caller that keeps using it. The
languages with moves make this a compile error; the languages without moves
share the container so both sides see every change; C++ leaves a
valid-but-unspecified husk behind.

```rae
func consume(xs: own List(Int)) ret Int {
  xs.add(value: 4)
  ret xs.length
}

let n: Int = consume(xs: xs)
log("caller sees {xs.length}, callee returned {n}")
```

## What Rae does

```
Main.rae:16:6: use of moved value 'xs': it was passed to a parameter declared 'own'
on a path reaching here, so the callee owns it now; assign 'xs' a new value before
using it again, or pass a copy (`"{xs}"`, or a `copy T` parameter) if the caller
needs it
```

`own T` is a move (`docs/ownership-model.md`): the callee owns the value and
frees it; the caller's binding is consumed. The check is dataflow — a move on
one branch of an `if` poisons the name after the `if`, a reassignment makes it
live again, and `spawn f(xs: data)` is not a move (the spawn site deep-copies
for the worker).

What it used to do was worse than a copy: the callee got the list, grew it
(reallocating the buffer), and freed it at exit, while the caller's `xs` still
pointed at the old storage — `xs.length` read 3 from a dangling struct.

## Elsewhere

| language | behaviour |
|---|---|
| Python | no moves; the callee mutates the shared list, the caller sees 4 |
| JavaScript | no moves; the callee mutates the shared array, the caller sees 4 |
| Java | no moves; the callee mutates the shared list, the caller sees 4 |
| Go | the callee appends to its own slice header; the caller usually sees 3 — a classic Go surprise |
| Rust | compile error: `use of moved value: xs` |
| C | no moves; whatever the pointer points at is shared |

## Expected outcome

Compile error containing `use of moved value 'xs'`.
