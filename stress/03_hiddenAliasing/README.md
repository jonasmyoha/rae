# 03 — Hidden aliasing

**Verdict: `handles`** — Rae prints `a.items 3  b.items 4`.

## The foot gun

Two names for one container. `b = a` looks like a copy, but in most languages
it binds a second name to the same list, so growing `b.items` also grows
`a.items` — and the author finds out much later.

```rae
var a: Box = { items: [1, 2, 3] }
var b: Box = a
b.items.add(value: 4)
```

## What Rae does

A struct holding a `List` is a value. `var b: Box = a` gives `b` its own list;
`a.items` still has three elements. There is no reference-vs-value distinction
to remember, and no `.clone()` to forget.

## Elsewhere

| language | behaviour |
|---|---|
| Python | `a.items` is also 4 — the assignment binds a second name to the same list |
| JavaScript | `a.items` is also 4 — objects are references |
| Java | `a.items` is also 4 — object references; a `.clone()` would be needed |
| Go | 4 when the append fits the shared backing array, 3 when it reallocates — it depends on capacity |
| Rust | compile error: `a` was moved into `b`; with `.clone()` it is 3, matching Rae |
| C | a struct copy copies the pointer inside it; both names see 4 |

## Expected outcome

Exit 0, stdout exactly `a.items 3  b.items 4`.
