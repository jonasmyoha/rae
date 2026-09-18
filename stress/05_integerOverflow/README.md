# 05 — Integer overflow and division by zero

**Verdict: `footgun`** — Rae prints two wrong numbers and exits 0.

## The foot gun

Arithmetic with no right answer: the largest `Int` plus one, and seven
divided by zero.

```rae
var big: Int = 9223372036854775807
big = big + 1
let bad: Int = 7 / 0
```

## What Rae does today

```
wrapped: -9223372036854775808
div0: 0
```

Exit code 0, nothing on stderr. The overflow wraps silently to the smallest
`Int`, and `7 / 0` evaluates to `0` — the C backend emits a guarded divide
that returns 0. Silently wrong with a clean exit is the worst outcome on
either shelf; no other language on this table answers 0.

## Elsewhere

| language | behaviour |
|---|---|
| Python | no overflow — arbitrary precision; `ZeroDivisionError` on `7 / 0` |
| JavaScript | no integer type — `2**63` loses precision as a double; `7 / 0` is `Infinity` |
| Java | wraps to `Long.MIN_VALUE`; `ArithmeticException` on `7 / 0` |
| Go | wraps; run-time panic on `7 / 0` (a constant `7 / 0` is a compile error) |
| Rust | panics in debug, wraps in release; always panics on `7 / 0` (constant: compile error) |
| C | signed overflow is undefined behaviour; `7 / 0` is undefined behaviour (usually `SIGFPE`) |

## Expected outcome (the current bug, asserted)

Exit 0, stdout exactly the two lines above, stderr empty. When the compiler
traps or diagnoses this, the runner prints `FIXED?` and this case must be
flipped to `handles`.
