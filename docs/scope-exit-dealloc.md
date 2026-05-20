# Scope-exit deallocation

Status: **design + Stage 1 (stdlib drop API) implemented**.
Owner: Rae compiler team.
Tracking test: `compiler/tests/cases/416_memory_leak_check`.

## Problem

In Rae's Live (bytecode VM) target, values bound to a `let` are
reachable only for that binding's lifetime — once the binding goes
out of scope, the VM's managed-value system reclaims any heap
storage they own. The Compiled (C backend) target doesn't do this.
`createList(T)`, `createStringMap(V)`, scene-parser allocations,
and anything else built on `rae_ext_rae_buf_alloc` stay pinned in
the heap forever.

Measured impact (M1 Max, Compiled target, `examples/98_mobile_ui`):
each `buildScreenWorld` rebuild leaks ~400 KB. 50,000 rebuilds → 20 GB
RSS. Test `416_memory_leak_check` confirms the same pattern on a
minimal `List(Int)` workload — ~8 KB per allocation pinned forever.

## Goal

The Compiled target should reclaim heap-owning values when their
owning binding goes out of scope, matching Live target semantics.
After this lands, `416_memory_leak_check` flips from FAIL to PASS
without any other changes.

## Non-goals (explicitly out of scope)

- **Arena allocators**: a downstream optimisation. The primitive
  this doc designs is per-binding scope-exit dealloc; arenas can
  ride on top later if the per-binding free overhead becomes a
  measurable cost in a real profile.
- **Reference counting / GC for the Compiled target**: this design
  uses linear ownership (move semantics) instead of refcounts.
  Refcount machinery is heavier and isn't needed for the cases
  we have today.
- **Cross-thread ownership transfers**: Rae's threading story is
  immature; revisit when threads land.

## Architecture (layered)

The work splits into five independently-shippable layers.

### Layer 1 — Runtime primitives (already exist, no change)

- `rae_ext_rae_buf_alloc(size, elemSize) -> Buffer(Any)` mallocs.
- `rae_ext_rae_buf_free(buf)` frees.

These are the only allocator-touching primitives in the stdlib.
Higher-level types (List, StringMap, IntMap, JsonDoc, etc.) compose
these.

### Layer 2 — Stdlib `drop()` API ✅ Stage 1

Hand-written `drop()` overloads in `lib/core.rae` for every heap-
owning stdlib type. Each one releases the type's `Buffer` field(s)
back to the allocator.

```rae
func drop(T)(this: mod List(T)) {
  rae_ext_rae_buf_free(buf: this.data)
}

func drop(V)(this: mod StringMap(V)) {
  rae_ext_rae_buf_free(buf: this.data)
}
```

Status: **implemented in Stage 1**.

Caller responsibility today: until Layer 3 lands, callers must
explicitly invoke `xs.drop()` at the end of any scope holding a
heap-owning local. This is what the test `417_explicit_drop_check`
demonstrates.

Future extension (Stage 2.1): for `List(T)` where `T` is itself
heap-owning, `drop` must iterate and recursively drop each element
before freeing the buffer. Today `T` is always a value type in the
98_mobile_ui codebase, so we ship the simple form first.

### Layer 3 — Codegen: emit `drop()` at scope exit 🚧 Stage 2

The C backend (`compiler/src/c_stmt.c`) gains an "implicit drop"
pass that runs at every scope exit:

- After every `ret` statement, just before the `return`.
- At the implicit end-of-body of a function with no trailing `ret`.
- (Stage 2.1) At every iteration boundary of a `loop` body for
  bindings declared inside the loop.
- (Stage 2.2) At every `}` closing a block (`if` / `else` /
  standalone `{}`).

For each `let x: T` in the surrounding scope:
- If `T` is a known heap-owning type (List, StringMap, …), emit
  `dropList_T(&x);` (the C-level mangled name) before the return.
- If `T` is `view` / `mod` (borrow), skip — the borrowed value
  belongs to another scope.
- If `T` is a primitive value type, skip — no-op.
- If `x` was moved out (see Layer 4), skip — caller owns it now.

The existing `defer` machinery is the closest precedent and the
right vehicle to extend. Drops can ride on the defer stack as an
"implicit defer" entry tagged with the binding name and type, then
flushed by the same code that flushes user-written defers.

### Layer 4 — Move semantics (skip drop when ownership transfers)

Linear ownership tracking: a heap-owning binding has exactly one
live owner. When ownership moves, the source's drop is suppressed.

Move-out points:
1. `ret x` — caller takes ownership.
2. `SomeStruct { field: x }` — struct constructor consumes `x`.
3. `f(x)` where `f` takes `T` by value (not `view`/`mod`).
4. Tuple / multi-return positions.
5. `someList.add(value: x)` where `x: T` — element-into-container.

Implementation: walk the function body once and tag each binding
with a "moved" flag if it appears in any of the above contexts.
Conservative: if we can't prove a binding was NOT moved, suppress
the drop (better to leak than double-free).

Detecting "moved" requires sema-level info about which parameters
are by-value vs by-borrow, which the existing sema already tracks
on `func` declarations. The codegen just consults that.

### Layer 5 — Struct field drops (recursive)

For every user-defined struct `type Foo { a: List(Int), b: String, ... }`,
the compiler synthesises:

```c
static void rae_drop_Foo(rae_Foo* this) {
  rae_drop_List_int64_t(&this->a);
  rae_drop_String(&this->b);
}
```

…and emits it once per type. The "is this field heap-owning?"
check recurses through composite types. Value-type fields (Int,
Float, Bool, fixed-size struct of values) contribute nothing.

Layer 5 makes Layer 3 useful for the music app: `UiWorld` contains
many `ComponentTable(T)`s, each of which contains a `List(Int)` +
a `List(T)`. Dropping `UiWorld` recursively cascades through every
nested heap-owning field.

## Implementation order

1. ✅ **Stage 1 — Stdlib `drop()` API** (`lib/core.rae`). Hand-written
   per-type drops. Manual-call only. Done in this commit.
2. ✅ **Stage 1.5 — `417_explicit_drop_check` test**. Proves the
   runtime/stdlib path is correct: same shape as 416 but calls
   `drop()` explicitly. Expected: `verdict: OK` on both targets.
3. 🚧 **Stage 2 — Codegen at function return**. Only top-level
   function lets, only at `ret` paths and end-of-body. No loop
   bodies, no nested blocks. After this, `416` passes the cases
   where the let is a top-level function local (which is most of
   them).
4. **Stage 3 — Move semantics**. Suppress drop for any binding
   that appears as the operand of a `ret`, in a struct constructor,
   or as a value parameter. Required before any complex stdlib
   function (createList, parseScene) survives the new codegen.
5. **Stage 4 — Loop / block scopes**. Emit drops on each iteration
   end and each `}` exit.
6. **Stage 5 — Struct field drops**. Synthesised per-type drop
   functions; cascading recursion.

Each stage ships independently and the test suite enforces that
the leak metric only ever goes down.

## Open questions

- **How do we handle `drop(T)` overloading vs the existing
  generics machinery?** The proposed `drop(T)(this: mod List(T))`
  form is a generic over `T`. Rae's existing generic specialization
  should produce the right instantiations as long as `drop` is
  treated like any other generic function. Verify when Stage 2
  lands and a function calling `add(value: ...)` etc. also pulls
  in `drop` instantiations.

- **Should `drop` be a special keyword or just a convention?**
  Today it's "just a function name". Layer 3 codegen looks up the
  function by name; if no `drop` overload exists for a given type,
  the binding is treated as a value type (no-op). Reserving the
  name `drop` as a special method keyword would be cleaner long-
  term, but the convention-based approach lets us land Stage 1
  with zero language changes.

- **`view`/`mod` refs into a dropped value**: Rae's borrow checker
  is supposed to prevent a `view`/`mod` reference outliving its
  source. We rely on that to avoid use-after-free in Compiled
  builds. If the borrow checker has gaps, Stage 2 will surface
  them as crashes; treat that as a forcing function to fix the
  borrow checker rather than to weaken the drop semantics.

## What `416` measures, what `417` measures

- **`416_memory_leak_check`** — no explicit drop. Fails on
  Compiled until Stage 2 (auto-emission) lands. Acts as the
  end-to-end regression guard.
- **`417_explicit_drop_check`** — explicit `drop()` call. Passes
  on both targets today, demonstrating that Layer 1+2 are
  correct. If anyone breaks `drop()` later, this test catches
  it independent of Stage 2 progress.
