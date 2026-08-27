# Aliasing rule for `viewAt` / `modAt` bindings into a List (design note, #645)

Status: **design note + recommendation** (investigate-first). Implementation is
tracked as **#658**. No behavior has changed yet.

## The hazard (confirmed, currently ALLOWED)

`viewAt` / `modAt` return `opt view T` / `opt mod T` — a raw pointer into the
List's backing storage (`&list->data[index]`). The pointer is captured **once**,
when the `if let` binding is established. Any structural mutation of the same
List inside the binding's live body can reallocate `data` (`add` past capacity
calls `grow()` → `realloc`), leaving the bound pointer **dangling**. A read or
write through it then touches freed memory.

Reproduction (compiles today with **no diagnostic**):

```rae
var xs: List(Int) = createList(Int, cap: 1)
xs.add(value: 5)
if let p: mod Int => xs.modAt(index: 0) {
  xs.add(value: 6)   # grows xs.data -> realloc -> p dangles
  xs.add(value: 7)
  p = 99             # writes through a dangling pointer into freed memory
}
# xs.copyAt(index: 0) is 5, NOT 99 — the write was silently lost (use-after-free)
```

Generated C (the mechanism, verbatim shape):

```c
rae_Mod_Int64 p = { .ptr = &__rae_list0->data[__rae_index0] };  // captured once
// ... rae_core_add_...(xs) calls here reallocate __rae_list0->data ...
*p.ptr = ((int64_t)99LL);                                       // UAF write
```

Runtime result: the write lands in the freed old buffer; `xs.data[0]` stays `5`.
This is undefined behavior (write to freed memory) that presents as a silent lost
write — and could corrupt the heap. **Value copies (`copyAt`) do not have this
hazard**: the copy is detached from the List's storage. The pointer is the
hazard, which matters now that #642 pushes `view`/`mod` as the recommended idiom
for large/heap-owning elements.

## What already protects the *loop* form

The range-collection loop already closes the equivalent hazard for its bodies.
`sema_expr_mutates_collection` / `sema_stmt_mutates_collection` (sema.c:1283 /
:1688) reject the structural mutators — `add`, `set`, `insert`, `remove`,
`clear`, `drop`, `grow`, `pop` — on the loop's source List inside the loop body,
with the diagnostic *"cannot mutate a List while a collection loop borrows its
elements"* (sema.c:1694). This is a conservative, lexical, compile-time check.

The gap: that borrow check is applied ONLY to `loop element in list` bodies. The
standalone `if let x: view/mod T => list.viewAt/modAt(index:)` binding form has
**no** such check — hence the reproduction above compiles. `docs/list-indexed-
access.md` §"Remaining lifetime work" already flagged this as required but
unimplemented.

## Options

**A. Borrow the container for the binding's scope (Rust-style) — RECOMMENDED.**
Extend the existing loop-body borrow check to the then-branch of an
`if let ...: view/mod T => list.viewAt/modAt(index:)` binding: while the binding
is live, reject the same eight structural mutators on the aliased List. Identify
the aliased List from the binding's source (the `viewAt`/`modAt` receiver) and
run `sema_stmt_mutates_collection` over the then-block, reusing the loop
diagnostic (reworded for the binding form).
- Pros: compile-time (no silent UAF ever reaches runtime); **zero runtime cost**;
  the machinery and diagnostic already exist; consistent with the shipped loop
  rule; needs **no new primitive** (Rae has no trap/panic — see the #642
  decision); conservative and lexical, matching the stated intent ("should
  remain a conservative local analysis, not grow into non-lexical lifetime
  inference").
- Cons: rejects some provably-safe programs (e.g. mutating a *different* List, or
  a call that only reads). Acceptable for a conservative borrow — the same
  trade-off the loop rule already makes. `copyAt` + `if let` is always available
  as the escape hatch when you must mutate while holding the value.

**B. Runtime generation-counter check.** Give `List` a generation counter bumped
on every structural mutation; `viewAt`/`modAt` snapshot it; each deref through
the reference checks the snapshot against the current generation and traps on
mismatch.
- Pros: permits more patterns (only the actually-invalidating case fails); the
  reference stays a pointer.
- Cons: per-List state + per-deref branch (runtime cost on the hot path #642
  optimized); fails only at **runtime**, not compile time; and it needs a
  trap/panic primitive, which Rae deliberately does not have (#642 decision). A
  worse fit for a language that prefers static rejection.

**C. Document as undefined.** Cheapest (a doc paragraph), no code.
- Rejected: shipping a silent use-after-free footgun in the *recommended* idiom
  is unacceptable. #642 steered code toward `view`/`mod`; that steer is only safe
  once this hazard is caught.

## Recommendation

Adopt **Option A**: extend the existing loop-body borrow check to
`viewAt`/`modAt` `if let` bindings. It is the smallest change that eliminates the
UAF at compile time, costs nothing at runtime, reuses shipped machinery and its
diagnostic, needs no new language primitive, and matches the conservative-lexical
policy the codebase already committed to for loops.

This must land before `view`/`mod` are broadly recommended — it blocks the
`view`/`mod` half of #650 for any site whose body mutates the container.

## Follow-up

Implementation tracked as **#658** (extend the borrow check to `viewAt`/`modAt`
`if let` binding bodies; test that mutating the aliased List in the body is a
sema error; `copyAt` + `if let` and mutation of a *different* List still allowed).
