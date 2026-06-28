# Bitwise operators

Status: **implemented**, 2026-06-27. Added to unblock the pure-Rae DEFLATE/PNG
codec (`docs/png-and-deflate-strategy.md`), but they are a general language
feature — useful for hashing, checksums, packing, `.raepack`, and any bit-level
work.

> **Breaking rename (2026-06-28):** the original Erlang-style names
> `band`/`bor`/`bxor`/`bnot`/`bsl`/`bsr` were replaced by
> `bitand`/`bitor`/`bitxor`/`bitnot`/`shl`/`shr`. **The old words are now
> available as ordinary identifiers** (variable/parameter/field/function/
> named-argument names) — not retained as aliases or contextual keywords.
> Semantics and precedence are unchanged; this was purely a spelling change.

## The operators (word operators)

Rae has **no symbolic bitwise operators** (`& | ^ ~ << >>`). Bitwise work uses
**word operators**, mirroring Rae's existing word-operator aesthetic (`and`,
`or`, `not`, `is`, `is not`):

| operator | meaning | underlying | arity |
|---|---|---|---|
| `bitand` | bitwise AND | `&`  | infix |
| `bitor`  | bitwise OR  | `\|` | infix |
| `bitxor` | bitwise XOR | `^`  | infix |
| `shl`    | shift left  | `<<` | infix |
| `shr`    | shift right | `>>` | infix |
| `bitnot` | bitwise NOT | `~`  | prefix |

All are **`Int`-only** (`Int` is a 64-bit signed integer). They produce `Int`.
Shift behavior (signed-`Int` `<<`/`>>`) is unchanged.

## Logical vs. bitwise: a clean split

The word/keyword distinction *is* the type distinction — no `|` vs `||` ambiguity:

- **`and` / `or` / `not`** — boolean logic, operate on `Bool`.
- **`bitand` / `bitor` / `bitxor` / `bitnot`** — bitwise, operate on `Int`.

`and` is *always* boolean; `bitand` is *always* bit-integer. There is no
operand-type overloading (unlike Pascal, where `and` is both). This keeps the
language explicit and analyzable.

## Precedence

Bitwise operators bind **tighter than arithmetic, comparison, and logical**:

```
x bitand 0xff is 0    # parses as (x bitand 0xff) is 0   — not x bitand (0xff is 0)
1 + 2 shl 4           # parses as 1 + (2 shl 4) = 33
```

This deliberately avoids C's infamous misdesign where `a & b == c` parses as
`a & (b == c)`.

**Mixing different bitwise operators without parentheses is a compile error** —
they share one precedence level and silent left-association is exactly the kind
of ambiguity Rae avoids:

```
1 bitand 2 bitor 3    # ERROR: mixing different bitwise operators requires parentheses
(1 bitand 2) bitor 3  # ok
1 bitand 2 bitand 3   # ok — same operator repeated is unambiguous
```

The canonical 0xRRGGBB pack reads cleanly with the required parens:

```
let packed: Int = (r shl 16) bitor (g shl 8) bitor b
```

## Both targets

The operators are implemented in **both** the Live (bytecode VM — opcodes
`OP_BITAND`/`OP_BITOR`/`OP_BITXOR`/`OP_SHL`/`OP_SHR`/`OP_BITNOT`) and Compiled (C backend
— `& | ^ << >> ~`) targets, and are constant-folded at compile time on integer
literals. Live and Compiled produce identical results (test
`compiler/tests/cases/525_bitwise_ops`).
