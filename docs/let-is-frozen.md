# `let` is frozen

**Status:** decided 2026-10-07 by the maintainer; ENFORCED since compiler
0.1.200. Writing into a `let` is a compile error. Step 1 measured the tree
(below); step 2 changed the 832 flagged declarations to `var` and made the
check an error. `RAE_FROZEN_LET=warn` downgrades it to a warning, only to
migrate code written before the rule.

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

Each is a compile error: `cannot modify 'outer.inner.value': 'outer' is a 'let',
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
  new name; they do not change the binding they read. The `if let` payload is
  itself a frozen `let`; `if var` is its mutable form (below).
- **`view` parameters** are unchanged: read-only, as today.

## Every value binding has a `var` form

Where a binding holds its OWN value, there is a frozen `let` and a mutable
`var` (maintainer decision 2026-10-07, compiler 0.1.201):

| frozen | mutable | binds |
|---|---|---|
| `let x: T = value` | `var x: T = value` | a value |
| `if let x: T = optional { … }` | `if var x: T = optional { … }` (also `} else if var`) | the unwrapped payload, owned by the branch |
| `loop let x: T in list { … }` | `loop var x: T in list { … }` | a COPY of each element |

`if var` is `if let` with a mutable payload: the same presence test, the
same move of the payload into the branch, the same drop at its end.

`loop var x: T in list` changes the copy only — the list keeps its element.
To change the elements in place, take them as aliases:
`loop let x: mod T in list { x.count = 0 }`.

An ALIAS (`view T` / `mod T`, bound with `=>` or as a loop element) has no
`var` form: its mode word already says whether it may write, and an alias is
bind-once. `if var x: mod T => place` and `loop var x: view T in list` are
parse errors (fixture 1020).

Before `if var` existed, the frozen-let migration wrote the mutable payload
as `if let openedX: T = … { var x: T = own openedX`; those 45 places (and
three older hand-written copies of the same shape) are now `if var`.

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

## Step 2: migration and enforcement (2026-10-07)

The compiler's message names each binding's declaration line, so the
migration was mechanical: the 832 `let` declarations behind the 2,095 sites
(in 314 files) became `var`; none needed restructuring. A rebuild of every
example, test case and stress case with the warning on then found no site
left, and the check became an error. Fixtures:
`1017_reject_frozen_let` (every forbidden form: member and nested member
writes, list/array element writes through `add`/`set`, a member and the whole
let as `mod` arguments, mutating method calls) and `1018_frozen_let_edges`
(`var`, `mod` parameters, a `mod` alias into a `var`, a `let` Buffer written
through, `if let` narrowing, `view` arguments).

A generic function body is checked per instantiation; a template that is
never instantiated is not analysed, so a write into a `let` there surfaces
when it is first used.

## A `view` is read-only the same way (0.1.247)

A `view` parameter or alias is someone else's value lent for reading, so
nothing reached through it may be changed either — the same rule as a
`let`, enforced at the same places. Three calls used to slip past it,
because sema paired arguments with parameters by position or never bound
the call: a generic whose `T: type` parameter shifted the pairing
(`push(Int, list: holder.numbers, …)`), a generic container method the
backend binds (`holder.numbers.add(…)`), and a generic whose `T` is
inferred. All three are errors now (fixture 1070, beside 929). An `own`
or `copy` parameter is exempt: the value was moved in or deep-copied at the
call, so it is the callee's to change. The C backend has emitted views as `const T*` since 0.1.246,
which is how 106's playback bindings, which wrote through a
`view PlaybackSystem`, were found.
