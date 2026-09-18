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

**Wraps two's-complement, in every build profile**: `Int.max + 1` is
`-9223372036854775808`. Computed in unsigned arithmetic so the wrap is
defined behaviour, not undefined behaviour an optimizer may assume away.
This is what every language with fixed-size integers does in production
(Java, Go, C#, Rust in release), and what a hash such as `lib/Noise`'s
32-bit finalizer relies on.

A dev-only overflow check (Rust's debug rule) was tried and dropped on
purpose: Rae has no dev/release semantic differences — a program that passed
its tests must behave identically when shipped — so the two builds differ
only in optimisation flags. See "One behaviour, every profile" below.

## One behaviour, every profile

The dev profile (`rae run --profile dev`, `-O0 -g`) and the release profile
(the default, `-O2 -DNDEBUG`) differ **only** in C compiler flags. Every
check the runtime makes — `Int` division, `List` bounds, `Array` bounds, the
crash handler — is made in both. Two exceptions existed in 2026-09 and were
removed the same month: `Array` dynamic-index checks compiled out of release
(never approved; `docs/value-aggregates-and-ownership.md` §1.7) and the
dev-only overflow trap above.

## What is NOT covered (yet)

- The helpers apply to `Int` (`Int64`). `Int32`, `Int8`, `UInt64` and the
  other widths keep the bare C operator: division by zero there is still
  undefined behaviour, and overflow wraps in every profile.
- `++` / `--` and compound forms that do not go through a binary `+`/`-`
  expression are plain C increments (they wrap as well).
- Shifts are unchecked.

## Crashes: the runtime's one line

The same "say what happened, in one line, then exit non-zero" rule covers
the faults the compiler cannot check for. A crash handler in the runtime
(`compiler/runtime/runtime_core_memory.c`) runs on an **alternate signal
stack** — on the main thread and on every spawned worker — so it can speak
even when the fault is the stack itself:

| fault | stderr | exit |
|---|---|---|
| stack overflow (a fault inside or just below the faulting thread's stack) | `rae: stack overflow (deep recursion?) in <program>` — one line, no backtrace | 128 + signal |
| any other bad memory access | `rae: segmentation fault (invalid memory access) in <program>`, then the `[rae crash]` C-level backtrace | 128 + signal |
| `SIGBUS` / `SIGFPE` / `SIGILL` / `SIGABRT` | `rae: bus error … / arithmetic fault / illegal instruction / abort in <program>`, then the backtrace | 128 + signal |

`<program>` is the entry `rae run` was given (`RAE_PROGRAM`, set by `rae run`)
or `argv[0]` of a built binary. `RAE_NO_CRASH_HANDLER=1` disables the handler
for a debugger that wants the raw signal. No-op on WASM, where the host
reports traps. Stress case `stress/09_deepRecursion` asserts the overflow line.
