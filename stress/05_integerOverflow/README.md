# 05 — Integer overflow and division by zero

**Verdict: `handles`** — division by zero is a runtime error with a source
position; overflow is defined per profile. (Flipped from `footgun`: it used to
print `0` for `7 / 0` and exit 0.)

## The foot gun

Arithmetic with no right answer: the largest `Int` plus one, and seven
divided by zero.

```rae
var big: Int = 9223372036854775807
big = big + one          # one == 1, computed at run time
let bad: Int = 7 / zero  # zero == 0, computed at run time
```

## What Rae does

```
wrapped: -9223372036854775808
…/Main.rae:19: runtime error: division by zero
```

Exit code 70 from the program (`rae run` reports 1). The rules
(`docs/integer-semantics.md`):

- `/` and `%` by zero are a **runtime error in every profile** — one line
  naming the file and line, exit 70, never a silent 0. `Int.min / -1` traps
  the same way.
- A divisor that is a **constant zero does not compile**: `7 / 0`,
  `x % (1 - 1)` and `x / zeroConst` are all rejected by sema, which is why
  this program computes its zero.
- `+ - *` overflow is a **runtime error in the dev profile**
  (`rae run --profile dev` stops at `big + one` with `integer overflow in +`)
  and **wraps two's-complement in release** — the first line above. The
  check where it is cheap to find the bug, the bare instruction where the
  inner loop runs; the same policy as array bounds.

## Elsewhere

| language | behaviour |
|---|---|
| Python | no overflow — arbitrary precision; `ZeroDivisionError` on `7 / 0` |
| JavaScript | no integer type — `2**63` loses precision as a double; `7 / 0` is `Infinity` |
| Java | wraps to `Long.MIN_VALUE`; `ArithmeticException` on `7 / 0` |
| Go | wraps; run-time panic on `7 / 0` (a constant `7 / 0` is a compile error) |
| Rust | panics in debug, wraps in release; always panics on `7 / 0` (constant: compile error) |
| C | signed overflow is undefined behaviour; `7 / 0` is undefined behaviour (usually `SIGFPE`) |

Rae's rules are Rust's, with Go's compile-time check for a constant divisor.

## Expected outcome

Exit 1 (via `rae run`), stdout exactly `wrapped: -9223372036854775808`,
stderr matching `Main.rae:<line>: runtime error: division by zero`.
