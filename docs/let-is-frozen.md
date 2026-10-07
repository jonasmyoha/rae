# `let` is frozen

**Status:** decided 2026-10-07 by the maintainer. Step 1 (this document, an
opt-in warning, the counts below) has landed; step 2 (migrate the flagged
code, make it an error) is queued.

## The three bindings

Rae takes Nim's model:

| keyword | what it is | can it change? |
|---|---|---|
| `const` | a compile-time value: literals, earlier constants, enum cases and arithmetic on them (no calls, no struct values) | never |
| `let` | a runtime value, **frozen as a whole** | nothing in it, at any depth |
| `var` | a runtime value | yes: rebinding, members, elements, `mod` arguments |

A `let` is frozen the way Rust's `let` and a Swift struct `let` are: Rae
structs are values (copied or moved, never shared by reference), so the
binding *is* the value, and freezing the binding freezes everything inside
it. Before this decision `let` only forbade rebinding, the shallow meaning of
Dart's `final` or Scala's `val`, which belongs to languages where a variable
is a reference to a shared object.

## What a frozen `let` forbids

With `let outer: Outer = ...`:

| form | example |
|---|---|
| writing a member or element | `outer.inner.value = 5`, `outer.items[i] = 2` |
| passing it, or a part of it, to a `mod` parameter | `bump(inner: outer.inner)` with `func bump(inner: mod Inner)` |
| calling a mutating method on it, or on a part of it | `numbers.add(value: 3)` on `let numbers: List(Int)` |

Each is reported as: `cannot modify 'outer.inner.value': 'outer' is a 'let',
which is frozen (docs/let-is-frozen.md); declare it 'var'` (and `cannot pass
'…' to 'mod' parameter '…'`, `cannot call mutating method '…' on '…'`).

A method call counts as mutating when the method it binds to takes
`this: mod T`. A generic container method (`List.add`) is bound by the C
backend rather than by sema, so for those every same-named method on the
receiver's type is consulted, and the call counts when all of them take
`this: mod`.

## What a frozen `let` does not touch

- **Handles freeze the handle, not the pointee.** A `let` holding a `Ptr`, a
  `Buffer`, a GPU resource id or an `EntityId` cannot be rebound, but writing
  *through* it (into the memory or resource it names) is allowed. The check
  stops at the first step through a `Ptr`/`Buffer` or a reference.
- **Aliases keep their own mode.** `let part: mod Inner => free.inner` is a
  `mod` alias: writes through it land in `free`, which must itself be a
  `var`. A `view` alias (or `view` parameter) stays read-only, as before.
- **Narrowing makes new bindings.** `if let value: T = maybe` and `=>` bind a
  new name; they do not change the binding they read.
- **`view` parameters** are unchanged: read-only, as today.

## Step 1: the measurement (2026-10-07)

`RAE_FROZEN_LET=warn` turns the check on. It is opt-in for this step because
the tree still has thousands of these writes: as a default-on warning it
would print hundreds of lines for every program (most sites are in `lib/`,
which every program compiles) and change the expected output of 130 test
cases. `compiler/tools/test-frozen-let.sh` (run by the suite) checks that the
three forms warn and the allowed forms do not, and that nothing prints
without the variable.

Unique sites found by building every example, every test case and every
stress case with `rae build --emit-c` and `RAE_FROZEN_LET=warn`:

| area | sites | files | member write | `mod` argument | mutating method call |
|---|---|---|---|---|---|
| `lib/` | 773 | 106 | 25 | 138 | 610 |
| `examples/` | 683 | 78 | 21 | 226 | 436 |
| `compiler/tests/` | 639 | 130 | 18 | 274 | 347 |
| `stress/` | 0 | 0 | 0 | 0 | 0 |
| **total** | **2,095** | **314** | **64** | **638** | **1,393** |

Most sites are the same shape: a `let` collection filled right after it is
made (`let verts: List(Float) = List.create(...)` then `verts.add(...)` or
`pushVert(verts: verts, ...)`), and a `let` world/resource handed to
functions that take it as `mod`. The fix in both cases is `var`. Sites in
`lib/` files that only the C backend compiles for some programs may be
missing from the count; step 2 rebuilds everything after the migration
anyway.

To list them again:

```sh
RAE_FROZEN_LET=warn rae build --emit-c --out /tmp/out.c <entry>/Main.rae 2>&1 | grep 'is frozen'
```

## Step 2 (queued)

Change every flagged `let` to `var` (or restructure so the mutation happens
on a short-lived `var` part), make the check an error by default, add
fixtures for each forbidden form and each allowed edge, and bump the
compiler's PATCH version: it is a breaking change.
