# 02 — Forgotten none check

**Verdict: `handles`** — Rae rejects the program at compile time.

## The foot gun

A lookup that may find nothing is used as if it always finds something. The
"not found" case is easy to forget, and in most languages the mistake is a
run-time crash far from the lookup — the null-pointer problem.

```rae
func find(xs: view List(Int), want: view Int) ret opt Int { … ret none }

let forced: Int = find(xs: xs, want: 9)
```

## What Rae does

```
Main.rae:18:25: optional not unwrapped — use `if let`
```

`opt Int` is a distinct type: a value or `none`, and nothing else can be an
`Int`. The only way to the value inside is `if let v: Int = find(…) { … }`,
so the missing case is always written out.

## Elsewhere

| language | behaviour |
|---|---|
| Python | runs; `None` reaches the caller and blows up later with `TypeError` / `AttributeError` |
| JavaScript | runs; `undefined` leaks and fails later as `undefined is not a function`, or becomes `NaN` |
| Java | runs; `NullPointerException` at the first use, far from the lookup |
| Go | runs; the zero value (`0`) or a nil-dereference panic, depending on the return type |
| Rust | compile error unless you write `.unwrap()`, which panics at run time on `None` |
| C | runs; a `NULL` or sentinel value is used as data — undefined behaviour |

## Expected outcome

Compile error containing `optional not unwrapped`.
