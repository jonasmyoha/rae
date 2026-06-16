# Stage 1 — structural drop: implementation plan

> **Status: complete.** Stage 1 structural drop is implemented in
> both backends. The C-side cascade machinery has been load-bearing
> since the May 2026 Phase 1–3 work; the Live VM equivalent landed
> across commits e31bb9b (descriptor infra), b25efde (scope-exit
> + early return), 173678d (reassignment cascade + leaf audit),
> c5b42b2 (leaf scope-exit + multi-ret move-marker fix), and the
> generic-spec commit that closes this plan. See the changelog
> section at the bottom for the per-commit summary.


> Scope per the design doc `drop-semantics-and-defer.md`:
>
> * recursively detect owned fields in structs and generic
>   specializations
> * synthesise cleanup for nested `String`, `List`, `Buffer`, and
>   structs containing them
> * run cleanup on normal scope exit and existing early-return
>   paths
> * support both C and Live VM backends
> * **no** user-defined `drop`, `defer`, explicit early drop, or
>   new copy/move rules yet

This plan grounds Stage 1 in the compiler as it stands today —
not as a clean-room build.

## Starting point: cascade-drop is already shipped in the C backend

Most of Stage 1's surface area already exists. The Phase 1–Phase 3
cascade-drop work landed in May 2026 and is currently load-bearing
in the mobile UI (drives the 5.5 MB / 20 k-iter leak plateau memory
documented in the project memory).

Concretely, the C backend (`compiler/src/c_backend.c`, plus call
sites in `c_stmt.c` / `c_call.c` / `sema.c`) already:

1. **Synthesises per-struct drop helpers.** For every non-generic
   user struct that transitively needs cleanup, the compiler emits
   two C functions:
   * `rae_drop_struct_<T>` — full cascade. Drops String fields,
     recurses into struct fields, drops List/Map fields.
   * `rae_drop_struct_<T>_alias` — strict cascade. Skips String
     fields and skips List/Map fields whose element type needs
     cascade. Called when the local was bound from a known
     alias-returning call (`buf_get`, `componentGet`, …).
2. **Synthesises per-T container drop helpers.** `drop(T)(this:
   mod List(T))`, same for `StringMap` / `IntMap`. Iterates
   elements and recurses through their cascade drop.
3. **Classifies locals as owning vs. aliasing** via
   `local_struct_owns_heap` in `c_stmt.c`. Auto-init, struct
   literal, and most call returns are flagged owning. A call
   whose body ends in a `buf_get`-flavoured return chain is flagged
   aliasing. The classifier recurses through `ret-of-call` patterns
   with cycle protection.
4. **Routes drop variant selection from the binding site.** The
   FULL variant runs at scope exit for owning locals; the ALIAS
   variant runs for aliasing locals. Never calls the FULL drop on
   a local bound from an aliasing extraction.
5. **Cleans up early-return paths.** Phase 7 / "early-return
   cleanup epilogue" task is marked completed; the codegen path
   walks live owned locals on every `ret` site.
6. **Reassignment cleanup for struct fields.** Task #79 added
   String reassign-drop for struct field targets.
7. **Owning-let deep-copy.** Task #82-#85 added a
   `type_needs_deep_copy` classifier and call-site deep-copy on
   `let` from an `IDENT` source, so `let b = a` doesn't silently
   alias the underlying heap.

This means **the "synthesise structural drop and run it at scope
exit" portion of Stage 1 already exists for the C backend.** The
mobile UI proves it works under stress.

What does **not** yet exist in tree:

* **Live VM equivalent.** `vm_emit_stmt.c` has zero cascade-drop
  emission. Live-mode currently relies on string-pool mark/take/
  flush and arena allocation for cleanup — it doesn't run per-struct
  drop helpers because there aren't any in the bytecode pipeline.
  Phase 1f mobile UI work and royalblush-rae both run in compiled
  mode for this reason.
* **A canonical, user-facing test surface.** Tests 443-449,
  460-464, 482-483 cover individual phases of cascade-drop, but
  they're labelled by phase number rather than as "Stage 1
  structural drop." The specific examples the Stage 1 spec calls
  out (Inner/Outer, several nesting levels, repeated UI-world
  cycles) may or may not be covered exactly.
* **The trivial-struct fast path is implicit, not explicit.** A
  struct with only `Int` / `Float` fields gets no
  `rae_drop_struct_<T>` emitted because
  `type_needs_cascade_drop` returns false. That's correct
  behaviour, but there's no test that asserts the helper is
  *absent* from the C output for trivial structs.

## What "shipping Stage 1" actually means from here

Three categories of work:

### A. Audit and catalogue what's in the C backend
*(no code changes, just documenting the load-bearing reality)*

Goal: a short reference in the docs that names each piece of the
existing cascade-drop machinery, its trigger condition, and which
tests pin it. Future maintainers should not have to spelunk through
`c_backend.c` to learn this.

Output: an addendum or update to this doc plus inline doc
comments where the codepath is dense.

Cost estimate: half a day.

### B. Live VM parity
*(the actual implementation work)*

Goal: Live-mode programs get the same per-struct cleanup behaviour
as compiled programs.

Approach (to be confirmed once the VM emitter is read in detail):

1. Mirror `type_needs_cascade_drop` on the VM side, using the
   same AST predicate. Doesn't need to be re-implemented —
   probably just shared with c_backend.c.
2. At each `let` site for an owning struct local, allocate a
   slot-table entry recording "this local needs drop on scope
   exit" plus the type. The string pool already uses a mark
   stack with mark/flush — likely model the per-scope drop list
   the same way: push on let, run on scope/early-return.
3. Emit a new opcode (or reuse `OP_CALL` to a synthesised native)
   that performs the cascade walk at runtime. Two options for
   how the walk is implemented:
   * **Option B1: native dispatcher per type.** The VM looks up
     `rae_vm_drop_struct_<T>` at registration time and calls it.
     Mirrors C codegen. Heaviest. Most efficient.
   * **Option B2: AST-driven generic walk.** A single native
     takes a `TypeId` and walks the struct's field list at
     runtime, dispatching on field type. Simpler to implement,
     slower per drop.
   * **Option B3: bytecode-emitted unrolled walk.** The compiler
     emits explicit `OP_DROP_FIELD` / `OP_DROP_LIST_ELEMS`
     sequences. Most uniform with the rest of the VM. No new
     natives.
4. Aliasing-let classification ports verbatim from c_stmt.c. The
   FULL/ALIAS variant choice happens at codegen time regardless
   of backend.
5. Container drop helpers (`drop(T)(this: mod List(T))` etc.)
   either become VM natives synthesised at registration time or
   become per-call inline bytecode walks.

Open sub-questions inside Option B:
* Does Live mode need full parity with the C backend's drop, or
  can it get away with "drop Strings + List buffers, leak the
  outer struct memory" (acceptable in Live because the VM arena
  resets on hot-reload)?
* Should this work block on a shared TypeId infrastructure
  between the two backends, or duplicate the predicate?

Cost estimate: hard to call without reading `vm_emit_stmt.c`
end-to-end. Probably 2–4 days of work, gated by which Option B
sub-path is chosen. Need to spike Option B3 (bytecode walk) on a
toy case before committing.

### C. Stage 1 test matrix
*(verifying the spec, end-to-end)*

The user-specified matrix, plus a couple of natural extensions:

```rae
type Inner {
  text: String
}

type Outer {
  inner: Inner
  names: List(String)
}
```

| # | Case | What it verifies |
| - | ---- | --- |
| 1 | `let o: Outer = createOuter()`, normal scope exit | structural drop fires on nested struct + list |
| 2 | 3-level nesting (`Outer{ middle: Middle{ inner: Inner } }`) | recursion past depth 2 |
| 3 | `let xs: List(Outer) = …` | List drop iterates and recurses |
| 4 | `let p: Pair(String, List(String)) = …` | generic struct specialisation |
| 5 | Struct with two containers (`List(String) + Map(String, Inner)`) | multiple owning fields in one struct |
| 6 | `if cond { ret }` early-return mid-function | early-return path runs drops |
| 7 | `let o: Outer = a` then `o = b` | reassignment: previous owned value gets cleaned up |
| 8 | `type Point { x: Float, y: Float }` | no `rae_drop_struct_Point` emitted at all |
| 9 | Repeated `createUiWorld()` / drop in a loop | leak check: RSS stable across many iterations |
| 10 | Cases 1–9, same source, Live VM target | Live parity |

Output: new test cases under `compiler/tests/cases/` numbered
sequentially after the existing 482/483. Cases 1–9 in
**compiled mode** first (regression coverage against what the C
backend already does); cases 1–10 in **Live mode** after Part B
lands.

Cost estimate: a day for cases 1–9 + per-case assertions; another
day to wire Live-mode parallel runs into the existing test
harness if needed.

## Suggested order

1. **B.0 read pass.** Read `vm_emit_stmt.c`, `vm_emit_expr.c`,
   `vm.c` and the existing OP_* set. Pick between Option B1 / B2 /
   B3 with a one-paragraph justification. Don't skip this step —
   the choice has knock-on effects on the rest of B.
2. **A. Catalogue the C-side state.** Worth doing before any
   Live-side work so the parity target is concrete.
3. **C cases 1–9 (compiled mode only).** These should already
   pass against today's compiler. If any of them fail, those are
   the first things to fix — they're the spec for Stage 1.
4. **B implementation.** Land Live-mode parity. Commit incrementally
   per opcode / per sub-feature.
5. **C case 10 (Live mode).** Add the parallel-run column.
6. **Documentation pass.** Update the design doc's "Decided"
   list — structural drop moves from "likely" to "implemented in
   both backends." Add a one-line stage marker in this plan.

## What's explicitly out of scope for Stage 1

To keep the plan honest:

* User-defined `drop` (Stage 4 in the design doc).
* `defer` (Stage 5).
* Explicit early drop syntax (Stage 2).
* Any new copy/move rules. Stage 1 inherits whatever the existing
  C backend does for `let b = a` and stays compatible with it.
* Resource-wrapper types (`Texture`, `File`). They keep working
  the way they do now — manual close/unload via `closeWindow` etc.
* Panic semantics.

## Open questions before code starts

1. **Option B1 vs B2 vs B3** for the Live VM. Spike needed.
2. **Whether B can ship with reduced fidelity** (drop the
   contained Strings/Lists, leak the outer struct frame) given
   Live mode's arena-reset hot-reload model. If yes, B shrinks
   considerably.
3. **Test numbering.** Continue the 484+ block, or carve a new
   500-block for the "Stage 1 structural drop" suite?
4. **Whether to factor the predicate.** `type_needs_cascade_drop`
   currently lives in `c_backend.h`. If both backends consume it,
   it should move to a backend-neutral header. Same for
   `local_struct_owns_heap`.

None of these block writing the doc or the cases-1-9 regression
suite. They block B implementation specifically.

## Recommendation

Implement in this order, one commit per item, asking for review
between each:

1. This plan (current doc) — done.
2. Spike report: read VM emitter, pick Option B1/B2/B3.
3. Doc the existing C-side cascade-drop architecture (Part A).
4. Add tests 1–9 against the C backend (Part C, partial).
5. Live VM cascade-drop implementation (Part B).
6. Add test 10 (Live parity row).
7. Promote structural drop from "likely" to "implemented" in
   `drop-semantics-and-defer.md`.

Stop after step 1 if anything in this plan reads wrong.

## Changelog — Stage 1 closure

| Commit | Scope |
| ------ | ----- |
| `e31bb9b` | Lifted classifiers to `ownership.{c,h}`; synthesised per-type FULL/ALIAS VM drop natives via `RaeVmDropDescriptor` + shared `drop_native_cb`. Tests 498. |
| `b25efde` | Wired scope-exit + early-return emission. Move-tracking on `ret <bare-ident>`. Tests 499. |
| `173678d` | Reassignment cascade (explicit drop before `OP_SET_LOCAL` for user-struct locals) + runtime audit confirming leaf reassignment already cleans up via `value_free` + `value_copy`. Tests 500. |
| `c5b42b2` | Scope-exit cleanup for bare leaf locals via `OP_DROP_LOCAL` (String, List(T), Buffer(T), StringMap(V), IntMap(V)). Fixed the multiple-return-site move-marker control-flow hazard (the `dropped` flag is no longer poisoned by the move-exclusion branch). Tests 501, 502. |
| `0a3023e` | Concrete generic struct specializations (Live VM side). `vm_compile_module` now drives the C backend's `discover_specializations_module` so `ctx->generic_types[]` is populated for the Live VM. `vm_drop_register_for_module` adds Pass 1b that walks the spec list and synthesises FULL/ALIAS descriptors per spec, using `rae_mangle_type_specialized` with the leading `rae_` prefix stripped for the helper key. `type_needs_cascade_drop` and `type_needs_deep_copy` now substitute generic params when recursing. The let-classifier probes the registry directly (instead of running a substitution-blind predicate), so non-generic structs, specs, leaves, and trivial specs all flow through one consistent gate. Tests 503. |
| _(this commit)_ | Concrete generic struct specializations on the **C backend** side. Closes the last semantic-equivalence gap: valid Rae source that uses `Wrapper(String)` / `Pair(...)` / similar now cascades identically under both targets. `c_backend.c` adds Pass A' to collect concrete specs from the same `ctx->generic_types[]` list (skipping leaf containers — they have their own per-T `drop(T)` mechanism). Each spec entry shares the existing `StructDropEntry` shape, so the existing body-emission loop (which was already substitution-aware via `gp` + `ga`) emits `rae_drop_struct_<MangledSpec>` and `..._alias` bodies for free. `c_stmt.c` drops its `if (type->generic_args) continue;` bailout so spec-typed locals get their helper called at scope exit. Stdlib generic containers (List / StringMap / IntMap) stay on the `is_drop_target_type` path that calls the user-defined `drop(T)` overload — only USER generic structs go through the new pass. Tests 504 (parity, runs on both targets). |

After step 7 the two backends apply structural drop with the same
gate. The original `c_backend.c:1236` "Generic-spec auto-collection
intentionally NOT done here" comment is no longer applicable and
has been replaced by Pass A'.
