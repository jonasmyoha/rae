# Integer arithmetic: what `Int` does at the edges

`Int` is a signed 64-bit integer. C leaves its edge cases undefined (signed
overflow, division by zero); Rae defines them, and the C backend routes every
`+ - * / %` on `Int` through small runtime helpers
(`compiler/runtime/rae_runtime.h`, "Int arithmetic") that implement the rules
below. Stress case `stress/05_integerOverflow` asserts them.

## Division and modulo by zero

**A runtime error, in every profile.** The program prints one line and exits:

```
path/to/Main.rae:19: runtime error: division by zero
```

Exit code **70** (sysexits `EX_SOFTWARE`: a defined trap, not a crash; `rae
run` reports it as 1 like any non-zero exit). `%` says `division by zero
(modulo)`. `Int.min / -1`, the one other divide that cannot be represented,
traps as `integer overflow: Int.min / -1`; `Int.min % -1` is 0.

Never a silent `0`, which is what the bare C divide happened to produce on
arm64 before this rule, and never a `SIGFPE`.

**A constant zero divisor is a compile error.** If the divisor folds to
integer zero at compile time — the literal `0`, an expression like `(1 - 1)`,
or a `const` — sema rejects the program:

```
Main.rae:8:18: division by zero: the divisor is the constant 0
```

A float divisor of `0.0` is not an error at either time: `7.0 / 0.0` is an
IEEE infinity, as everywhere else.

## Overflow of `+`, `-`, `*`

Profile-dependent, the same policy as array bounds checks:

| profile | `Int.max + 1` |
|---|---|
| dev (`rae run --profile dev`, `-O0 -g`) | **runtime error** `integer overflow in +` (`-`, `*` likewise), exit 70 |
| release (the default, `-O2 -DNDEBUG`) | **wraps** two's-complement: `-9223372036854775808` |

The dev check is one compiler builtin (`__builtin_add_overflow`) per
operation; the release path computes in unsigned arithmetic so the wrap is
defined behaviour, not undefined behaviour an optimizer may assume away.

This is Rust's rule (debug panics, release wraps), with Go's compile-time
rejection of a constant zero divisor.

## What is NOT covered (yet)

- The helpers apply to `Int` (`Int64`). `Int32`, `Int8`, `UInt64` and the
  other widths keep the bare C operator: division by zero there is still
  undefined behaviour, and overflow wraps in every profile.
- `++` / `--` and compound forms that do not go through a binary `+`/`-`
  expression wrap in every profile.
- Shifts are unchecked.
