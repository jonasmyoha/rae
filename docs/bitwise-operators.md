# Bitwise operators

Status: **implemented**, 2026-06-27. Added to unblock the pure-Rae DEFLATE/PNG
codec (`docs/png-and-deflate-strategy.md`), but they are a general language
feature — useful for hashing, checksums, packing, `.raepack`, and any bit-level
work.

## The operators (Erlang-style words)

Rae has **no symbolic bitwise operators** (`& | ^ ~ << >>`). Bitwise work uses
**word operators**, mirroring Rae's existing word-operator aesthetic (`and`,
`or`, `not`, `is`, `is not`):

| operator | meaning | arity |
|---|---|---|
| `band` | bitwise AND | infix |
| `bor`  | bitwise OR  | infix |
| `bxor` | bitwise XOR | infix |
| `bsl`  | shift left  | infix |
| `bsr`  | shift right | infix |
| `bnot` | bitwise NOT | prefix |

All are **`Int`-only** (`Int` is a 64-bit signed integer). They produce `Int`.

## Logical vs. bitwise: a clean split

The word/keyword distinction *is* the type distinction — no `|` vs `||` ambiguity:

- **`and` / `or` / `not`** — boolean logic, operate on `Bool`.
- **`band` / `bor` / `bxor` / `bnot`** — bitwise, operate on `Int`.

`and` is *always* boolean; `band` is *always* bit-integer. There is no
operand-type overloading (unlike Pascal, where `and` is both). This keeps the
language explicit and analyzable.

## Precedence

Bitwise operators bind **tighter than arithmetic, comparison, and logical**:

```
x band 0xff is 0      # parses as (x band 0xff) is 0   — not x band (0xff is 0)
1 + 2 bsl 4           # parses as 1 + (2 bsl 4) = 33
```

This deliberately avoids C's infamous misdesign where `a & b == c` parses as
`a & (b == c)`.

**Mixing different bitwise operators without parentheses is a compile error** —
they share one precedence level and silent left-association is exactly the kind
of ambiguity Rae avoids:

```
1 band 2 bor 3        # ERROR: mixing different bitwise operators requires parentheses
(1 band 2) bor 3      # ok
1 band 2 band 3       # ok — same operator repeated is unambiguous
```

The canonical 0xRRGGBB pack reads cleanly with the required parens:

```
let packed: Int = (r bsl 16) bor (g bsl 8) bor b
```

## Both targets

The operators are implemented in **both** the Live (bytecode VM — opcodes
`OP_BAND`/`OP_BOR`/`OP_BXOR`/`OP_BSL`/`OP_BSR`/`OP_BNOT`) and Compiled (C backend
— `& | ^ << >> ~`) targets, and are constant-folded at compile time on integer
literals. Live and Compiled produce identical results (test
`compiler/tests/cases/525_bitwise_ops`).
