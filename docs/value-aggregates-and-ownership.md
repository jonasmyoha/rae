# Fixed-size value aggregates + checked ownership transfer

Status: **design, approved for implementation as #352/#353/#354.** No
compiler code is written under this document (#351 is design only).

Parent: `language-readiness-for-renderer.md` §3, §4, Track A. These two
features are co-designed because they meet at exactly one place — what
happens when a value aggregate is put into a container — and that is the
place the renderer will lean on hardest.

---

## Part 1 — Fixed-size value aggregates

### 1.1 The requirement

`Mat4` must be 16 floats by value, in registers or on the stack, with no
allocation. Today `lib/math3d.rae` uses `List(Float)` and `mat4Mul`
heap-allocates per call; `gpu3d.modelMatrix()` costs eight allocation/free
pairs per object per frame. Rae has no way to say "N of T, by value".

### 1.2 Spelling: `[N]T`

```rae
type Mat4 { m: [16]Float }
type Quat { v: [4]Float }
var palette: [128]Mat3x4
```

Chosen over `Array(T, N)` because Rae's generic machinery substitutes
**types**, not values — `N` in a generic argument slot would be a new kind
of thing in an existing position. `[N]T` puts the count in its own syntactic
slot and reads consistently with Rae's other prefix type modifiers (`view
T`, `mod T`, `opt T`).

`N` is a **const-expression of type `Int`**: an integer literal or a `const`
binding, evaluated at compile time. Not a runtime value, not a generic
parameter in v1. `[16]Float` and `[maxJoints]Mat3x4` are both legal where
`const maxJoints: Int = 128`.

Rejected alternatives, for the record:
- `T[N]` (C-style postfix) — collides visually with indexing and reads
  badly under the existing prefix modifiers (`view Float[16]`).
- Making it generic (`Array(T)(N)`) — needs value-generics, a much larger
  feature, for no benefit here.

### 1.3 A distinct type kind, not sugar over `Buffer`

Add `TYPE_ARRAY` to `TypeKind` in `compiler/src/type.h`:

```c
struct { TypeInfo* base; size_t count; } array;
```

**Not** a compile-time-length `Buffer(T)`. `TYPE_BUFFER` is a *pointer to
heap storage* — it carries no length in the type, and its whole purpose is
indirection. Reusing it would make "is this by value?" a property the
compiler cannot answer structurally, and every one of `ownership.c`'s
classification helpers (`is_drop_target_type`, `type_owns_heap_storage`,
`type_needs_cascade_drop`, `type_needs_deep_copy`) needs exactly that
answer. A distinct kind lets all four stay simple.

### 1.4 C lowering: wrapped in a struct, deliberately

```c
typedef struct { float v[16]; } rae_Array16_Float;
```

**Not** a bare `float[16]`. Bare C arrays decay to pointers on assignment
and parameter passing, so `a = b` would copy a pointer and silently
reintroduce aliasing — precisely the bug class Part 2 exists to remove.
Wrapping in a struct gives real by-value assignment, parameter passing and
return, with identical memory layout and no ABI cost. This also makes
`Vec4`/`Quat` a contiguous 16-byte value, which is the precondition for the
SIMD lowering in #357.

### 1.5 Value semantics and drops

- **Copy**: assignment and parameter passing copy the whole aggregate. For
  primitive `T` this is a `memcpy`/struct assignment the C compiler will
  inline or vectorise.
- **Drop**: `[N]T` is a drop target **iff `T` is**. For primitive `T` —
  `Float`, `Int`, `Bool` — there is no drop, no cascade, no per-element
  loop. This is the renderer case and it must be exactly free.
- **Cascade**: when `T` does own heap storage (e.g. `[4]String`), drop and
  deep-copy iterate elements, reusing the machinery
  `type_needs_cascade_drop` already drives.
- **Ownership modes**: `view [16]Float` passes a pointer (no copy) — the
  right default for a large aggregate read-only. `own`/`copy` behave as for
  any value type. A `[N]T` of primitives never *needs* `own`, since copying
  is total.

### 1.6 Bounds policy: constant-checked always, dynamic-checked in debug

Three cases, deliberately different:

| Index | Debug | Release |
|---|---|---|
| Compile-time constant | **compile error** if out of range | compile error |
| Dynamic | runtime check, abort with location | **unchecked** |

Constant indices dominate matrix code (`m[5]`, `m[10]`) and verifying them
costs nothing, so an out-of-range constant should never reach runtime.
Dynamic indices in a per-joint inner loop must not pay a branch in release
builds. This is a stated policy, not an accident — and it is the one part of
this design where a later reversal (always-checked) would be a performance
regression, so it is called out for explicit approval.

### 1.7 Interaction with existing generics

`List([16]Float)` must work: the element is a value type of statically known
size, so the existing `Buffer` element stride (`sizeof(T)`) is correct
without change. This combination — a value aggregate inside a container — is
exactly what Part 2's tests must cover, because it is what a skinning
palette and a draw-command buffer both are.

---

## Part 2 — Checked ownership transfer

### 2.1 What exists today (audited, not assumed)

- `compiler/src/ownership.c` is **110 lines and contains no diagnostics**.
  It provides four classification predicates and nothing else. There is no
  ownership analysis pass.
- Ownership is resolved **during code emission**: `c_call.c` decides per
  argument whether to deep-copy, move, or pool-take.
- Move state is `bool local_moved[256]` on the **codegen** context
  (`c_backend_internal.h`), used only to suppress scope-exit drops. It is
  per-function, flow-insensitive, and capped at 256 locals.

So "make it a compile error" is **adding a check that does not exist**, not
tightening one. That reframing is the most important input to the design
below, and it argues strongly against an ambitious solution.

### 2.2 Decision: a targeted call-site rule, not a borrow checker

**We will not build flow-sensitive ownership analysis.** A borrow checker
needs a dataflow lattice, branch merging, and loop fixpoints; `local_moved`
is a flat bool array. Building that is a project in itself and is not what
the renderer needs.

Instead, one rule, checkable from types and expression kinds at the call
site, with no flow analysis:

> **The own-argument rule.** Where a parameter is declared `own T` and `T`
> owns heap storage, the argument must be an *owning expression*.

**Owning expressions** (accepted):

| Form | Example |
|---|---|
| Call / method-call result | `makeName()` |
| Interpolation or concat result | `"{a}"`, `a.concat(other: b)` |
| Object / struct literal | `Entity { id: ... }` |
| Explicit move | `own x` |
| Explicit copy | `copy x` |
| A local `let`/`var` being moved | existing move path |

**Non-owning expressions** (rejected with a diagnostic):

| Form | Why |
|---|---|
| A `view T` / `mod T` parameter, bare | the callee does not own it |
| A member read through a view | owned by whatever the view points at |
| A global | outlives the callee |

The diagnostic should name the fix explicitly: *"pass `copy x` to give the
callee its own value, or `own x` to transfer ownership"*.

### 2.3 Why this rule and not something stronger

It catches **all three** known hazards, which is the bar that matters:

1. **`log_overlay`** — `logLine(text: view String)` passing `text` to
   `List.set(value: own T)`. Rejected: bare `view` parameter. The fix
   becomes `copy text`, replacing today's `"{text}"` interpolation trick.
2. **#222** (already an open queue item asking for exactly this diagnostic)
   — `let x: String = someGlobal` then passing `x` to an `own String`
   parameter frees the global's buffer and dangles it. Rejected: the
   initialiser is a global, so `x` is an alias, not an owner. Note this
   case needs a small amount of local reasoning — see §2.4.
3. **`List(Struct)` deep-copy gap** — see §2.5; the rule plus §2.5 together
   close it.

It is also *cheap*: a type test and an expression-kind test per argument, in
sema, where source locations exist. No new IR, no analysis pass.

**What it deliberately does not catch:** use-after-move within a function
(`own x` then read `x`). That needs flow analysis. Accepted as out of scope
— it is a different bug class, it is not what bit us, and codegen's existing
`local_moved` already suppresses the double-drop in the common case.

### 2.4 One-hop alias tracking (needed for #222, and only that)

#222's shape is `let x: String = someGlobal` followed by `f(param: x)`. At
the call site `x` is a plain local, which §2.2's rule would accept.

The minimum addition that catches it, without flow analysis: when a `let`
binding is initialised **directly** from a non-owning expression (a global,
a `view`/`mod` parameter, or a member read through one), record that on the
binding's symbol. The rule then treats such a binding as non-owning.

This is one bit per local, populated at declaration, and it is *not*
dataflow — it never changes after the binding is created, and a `var`
reassigned later simply keeps the conservative answer. If that proves too
conservative in practice, the escape hatch is the same as everywhere else:
write `copy x`.

**Scope limit:** one hop. `let a = global; let b = a; f(param: b)` is not
required to be caught in v1. Note this explicitly so the gap is known
rather than assumed closed.

### 2.5 The `List(Struct)` deep-copy gap

`type_needs_deep_copy` already exists and answers correctly; the bug is that
the container path does not consult it. When `add`/`set` receive an owning
argument of a type that needs deep copy, they must perform the same deep
copy `copy` would — not a shallow struct assignment that duplicates interior
heap pointers.

This is a codegen fix in the container path, gated by an existing predicate.
It is listed here because it is the same hazard viewed from the callee side,
and because 2.2's rule alone would not fix it.

### 2.6 Where the check lives

**Sema**, after type resolution, alongside the existing argument checks in
`sema_analyze_expr`'s call handling. Not the backend: diagnostics need
source locations, and the check must run even for code paths codegen might
not reach.

---

## Part 3 — Test list (the deliverable for #353)

Grouped by what each proves. Every test is a compiler test case under
`compiler/tests/cases/`.

### A. Own-argument rule — rejections (must fail to compile)

1. `view String` parameter passed to `List.add(value: own T)` — the
   `log_overlay` shape.
2. `mod String` parameter passed to an `own String` parameter.
3. Global passed directly to an `own String` parameter — #222.
4. `let x: String = someGlobal` then `x` passed to `own String` — #222's
   exact reported shape, via the §2.4 alias rule.
5. Member read through a `view` struct passed to `own`.
6. Same, for a user type owning a `String` field (not just `String`).

### B. Own-argument rule — acceptances (must compile and run clean)

7. `copy x` into `own T` — the sanctioned fix; asserts independence
   (mutating the original does not affect the container).
8. `own x` into `own T` — transfer; asserts the source is not double-dropped.
9. Call result into `own T`.
10. Interpolation into `own T` — must remain legal (this is #343's path).
11. Object literal into `own T`.
12. Primitive into `own Int` — the rule must not fire for non-heap types.

### C. `List(Struct)` deep copy

13. `List(T)` where `T` has a `String` field: add, mutate the source, assert
    the stored element is unchanged.
14. Same, overwrite via `set`, assert no double free at teardown.
15. Nested: `T` contains a `List(String)`; assert full cascade.

### D. Value aggregates through containers (the renderer combination)

16. `List([16]Float)` — add, read back, assert values and zero aliasing.
17. `[N]T` embedded in a struct stored in a `List`; assert by-value copy.
18. `[4]String` (aggregate of heap elements) — assert element cascade drop
    and no leak.
19. Aggregate returned from a function and moved into a container.

### E. Value semantics

20. Assignment copies: `var a = b; a[0] = 9;` leaves `b[0]` unchanged.
21. Parameter passing copies for `own`/`copy`; `view` does not.
22. Constant out-of-range index is a **compile error**.
23. Dynamic out-of-range index aborts in debug with a useful location.
24. `[N]T` of primitives triggers no drop code. Verified the way 416/417
    verify drops — allocator/RSS behaviour at runtime, not by reading
    emitted C.

### F. Regression guards

25. The three existing workarounds, rewritten naturally, still pass:
    `frame_stats` without a ring buffer, `log_overlay` with `copy`, and
    `math3d` on value types.
26. Memory-plateau test (in the shape of 432) over a transform-heavy loop:
    RSS flat across 5000 iterations.

---

## Part 4 — Open questions for approval

1. **Bounds policy (§1.6)** — unchecked dynamic indexing in release is a
   deliberate performance choice. Approve, or require always-checked?
2. **`copy` as the sanctioned fix** — this makes `copy` appear in ordinary
   container code (`list.add(value: copy name)`). Acceptable ergonomics, or
   should `add` take `copy T` and copy implicitly?
3. **Scope of the rule** — apply to all `own T` parameters, or only where
   `T` owns heap storage? This document assumes the latter (primitives
   unaffected), which keeps the rule invisible in numeric code.
