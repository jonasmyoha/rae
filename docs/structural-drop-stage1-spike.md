# Stage 1 spike — Live VM cascade-drop strategy

> Companion to `structural-drop-stage1-plan.md`. Decides between
> the three implementation options for Live-VM cascade-drop (B1
> native-per-type, B2 generic walk, B3 bytecode-emitted unroll).
> Goal: **full observable parity** between Live and compiled
> backends. Live and compiled Rae should never silently disagree
> on ownership behaviour.

## How the Live VM works today (the relevant parts)

### Locals, scopes, function epilogue

* CallFrame holds a fixed `locals[256]` array
  (`compiler/src/vm.h:98`). No live-set, no per-scope drop
  tracking. The compiler counts emitted locals via
  `compiler->local_count` and *resets the count* at scope exit
  (`compiler/src/vm_emit_stmt.c:382, 395, 517`). The slot data
  is **not** freed at that point — the slots are simply
  considered unallocated.
* The only existing cleanup hook is `value_free()` inside
  `OP_BIND_LOCAL` and `OP_SET_LOCAL` (`compiler/src/vm.c:499,
  517–518`). These free the *previous* slot value when a slot
  is rebound. That covers reassignment within the same slot
  (`x = y`), but **not** scope exit, because at scope exit the
  slot is abandoned rather than overwritten.
* `emit_return()` (`compiler/src/vm_compiler.c:961`) emits
  `OP_RETURN` directly. The only thing between the return
  expression and the opcode is `vm_emit_defers()` for any
  scoped `defer` statements (`compiler/src/vm_emit_stmt.c:312,
  358`). **No cleanup sequence is emitted today.** This is the
  primary gap.

### Existing String / container cleanup

* No string pool in the VM; the `rae_string_pool_mark/take/
  flush` machinery is C-runtime-only. A grep for `string_pool`
  in `compiler/src/vm*.c` returns nothing.
* `OwnedString` values (`compiler/src/vm_value.h:24`) carry a
  `uint8_t* chars` allocated from the C heap. Their lifetime is
  bound to the containing `Value`; `value_free()` releases the
  buffer.
* `VAL_ARRAY` / `VAL_BUFFER` / `VAL_OBJECT` work the same way —
  each value is `value_free()`d when its slot gets rebound.
* No opcodes exist for explicit cleanup. Greps for `OP_DROP`,
  `OP_FREE`, `OP_CLEANUP`, `OP_RELEASE` in `vm.h` come back
  empty.

### Reassignment cleanup

* `OP_SET_LOCAL` already runs `value_free(slot)` then writes the
  new value (`compiler/src/vm.c:499`). So `let x = a; x = b`
  cleans up `a`.
* Field reassignment (`x.field = b`) uses `OP_SET_FIELD`
  (`compiler/src/vm.c:824+`), which performs the same top-level
  cleanup.
* What's missing: *cascade* cleanup. The top-level value is freed
  via `value_free`, but if that value owns nested heap data
  (`String` inside a struct, struct inside a list), nothing
  recursively releases the nested allocations. The C backend
  threads this via its synthesised `rae_drop_struct_<T>` helpers;
  the Live VM has no equivalent.

### Hot-reload

* `vm_patch.c` rewrites bytecode and relocates instruction
  offsets (`compiler/src/vm_patch.c:74–172`).
* `vm_registry_reload()` (`compiler/src/vm_registry.c:247–255`)
  frees the old chunk and installs the new one, but **native
  registrations persist** (`VmNativeEntry` array is owned by the
  registry, not the chunk). This matters for B1: per-type drop
  natives stay registered across reloads.
* Type metadata (`compiler/src/vm_registry.h:69`) does *not* get
  rebuilt on hot-reload. This matters for B2: any per-type
  metadata table for a generic drop walker would need explicit
  re-registration.

### Stress proof

* `compiler/tests/cases/416_memory_leak_check/main.rae` allocates
  ~5000 lists in a loop. Passes in Live today — because the
  loop's per-iteration reassignment triggers `value_free` on the
  previous value via `OP_BIND_LOCAL`. That mechanism does *not*
  cascade into nested struct fields; the test happens to exercise
  only top-level `List<Int>` which doesn't need cascade. A test
  that allocates a `List(String)` inside an `Outer` struct inside
  a loop is the right next probe.

## Shared backend-neutral classification (yes, lift it)

The C backend's two classifiers — `type_needs_cascade_drop` and
`local_struct_owns_heap` — both currently live in
`compiler/src/c_stmt.c`. Their only dependencies are `AstModule`,
`AstTypeRef`, and `CompilerContext`. Lifting them out is
straightforward and unblocks any Live-VM implementation:

1. Move both to a new `compiler/src/ownership.h` /
   `compiler/src/ownership.c` (or `compiler/src/cascade_drop.{c,h}`
   if more specific).
2. Same for `type_needs_deep_copy` (the C backend's deep-copy
   classifier, structurally parallel and currently colocated).
3. Both backends include the new header. No churn beyond the
   move + include adjustments in `c_stmt.c` / `c_backend.c`.

**This should happen before the Live implementation begins.** It
guarantees the two backends agree on "does this type need
cleanup?" without round-tripping through ad-hoc duplicated
logic.

## The three options, scored against full-parity Live VM

For each, the question is: how does the VM run the equivalent of
the C backend's `rae_drop_struct_<T>(&local)` at every scope-exit
or early-return site where the C backend would?

### B1 — per-type native drop dispatcher

**Shape.** For each struct/list/map type that needs cascade
cleanup, the compiler synthesises two native functions
`rae_vm_drop_struct_<T>` (FULL) and `rae_vm_drop_struct_<T>_alias`
(ALIAS). They're registered at module load via
`vm_registry_register_native()`. At each scope-exit and early-
return site, the compiler emits an `OP_GET_LOCAL <slot>` +
`OP_NATIVE_CALL "rae_vm_drop_struct_T"` pair.

**What changes:**
* `compiler/src/vm_compiler.c` — new pass that synthesises the
  native bodies (mirror of `c_backend.c`'s drop-helper emitter).
* `compiler/src/vm_natives_core.c` — generic dispatcher that the
  synthesised entries route through, or per-type entries
  registered dynamically.
* `compiler/src/vm_emit_stmt.c` — scope-exit and `ret` emission
  sites add the cleanup sequence between expression eval and
  `OP_RETURN`/scope reset.
* No new opcodes.

**Score:**
* Existing infrastructure: **strong**. Native registration is
  a well-tested path (raylib bindings, all stdlib core natives,
  Phase 1f scene resolver). No new mechanism required.
* Bytecode size: **best**. One opcode pair per dropped local.
* Native registry: per-type cost. A program with N cascade-
  droppable types adds N native entries. In practice a few
  dozen.
* Hot-reload: **clean**. Natives outlive chunk patches.
* Symmetry with C backend: **highest**. Same function naming,
  same FULL/ALIAS split, same trigger conditions. Trivial to
  diff cleanup behaviour between backends.
* Implementation effort: moderate. The per-type-emitter pattern
  is well-trodden. Once one type works, the rest follow.
* Testability: per-type natives can be invoked directly in unit
  tests.

### B2 — generic field-walking native

**Shape.** A single native `rae_vm_drop_struct_generic` accepts
a type-id (or type-name string) and walks the struct's field
list at runtime by consulting compiler-emitted metadata. At
scope exit, emit `OP_NATIVE_CALL "rae_vm_drop_struct_generic"`
with the type-id as an argument.

**What changes:**
* `compiler/src/vm_value.h` — `VmFieldNamesResolver`
  (`vm_value.h:93`) and `vm_registry.h:69`'s `type_metadata`
  field would have to be populated for every cascade-droppable
  type. Today these mechanisms exist but are sparsely used
  (`toJson` / `fromBinary` only).
* `compiler/src/vm_natives_core.c` — one generic walker.
* `compiler/src/vm_compiler.c` — emit metadata-registration calls
  at module load.

**Score:**
* Existing infrastructure: **weak**. The metadata machinery
  exists but is sparse. A lot of plumbing needed before the
  generic walker has the information it needs.
* Bytecode size: best (one opcode pair per drop site, same as
  B1).
* Native registry: smallest — one entry total.
* Hot-reload: **fragile**. Type metadata must be re-registered
  on every reload, with extra care not to leak the previous
  metadata.
* Symmetry with C backend: **medium**. Same trigger conditions,
  but the cleanup body diverges (interpreted walk vs. inlined
  code).
* Implementation effort: highest. Building out type metadata
  is a separate, substantial project.
* Testability: hardest. The walker's behaviour is data-driven;
  unit tests need to build mock metadata.

### B3 — bytecode-emitted unrolled cleanup

**Shape.** No new natives. The compiler emits, at every scope-
exit site, an explicit sequence of operations that walks the
local's fields and calls existing primitives for each leaf:
`OP_GET_LOCAL <slot>`, `OP_GET_FIELD <field_idx>`,
`OP_NATIVE_CALL "rae_string_drop"`, etc. Recursive struct drops
either nest inline (large bytecode) or call back into a smaller
per-type native (which makes B3 indistinguishable from B1 for
recursion).

**What changes:**
* `compiler/src/vm_emit_stmt.c` — new emitter that, given a type,
  walks its field tree and emits the unrolled cleanup.
* New leaf primitives (`rae_vm_string_drop`,
  `rae_vm_list_drop_elems`, …) as natives. These are simpler
  than full per-type cascade natives but still need to exist.
* Possibly new opcodes if a sufficiently fast cleanup is wanted
  (`OP_DROP_STRING <slot>` could avoid the `OP_GET_LOCAL` +
  `OP_NATIVE_CALL` overhead).

**Score:**
* Existing infrastructure: **medium**. Reuses field access
  opcodes but needs leaf primitives. "No new opcodes" is only
  true if performance isn't a concern.
* Bytecode size: **worst**. Each scope-exit emits one drop
  sequence per local, each sequence touches each field. A
  deeply nested struct produces dozens of opcodes per drop
  site.
* Native registry: small (a handful of leaf primitives).
* Hot-reload: clean (bytecode patches normally).
* Symmetry with C backend: **lowest**. The C backend uses
  per-type helpers; B3 deliberately unrolls. Diffing cleanup
  behaviour between the two becomes harder.
* Implementation effort: moderate-to-high. The unrolling logic
  has to handle every nesting case correctly. Recursion has
  to terminate via per-type natives anyway — which is B1.
* Testability: bytecode-level traces.

## Recommendation: **B1**

Targeted at the Stage 1 goal of full observable parity, B1 wins
on the dimensions that matter most for *this* VM:

1. **Mirrors the C backend's mental model.** Both backends end up
   with a `rae_drop_struct_<T>` and a `rae_drop_struct_<T>_alias`
   per cascade-droppable type, dispatched by FULL/ALIAS choice at
   the binding site. If you understand one, you understand the
   other. Parity is a sanity-check away — diff the lists of types
   that get drop helpers and assert equality.
2. **Reuses the most well-tested VM extension point.**
   `vm_registry_register_native()` is the path raylib, the stdlib
   natives, the Phase 1f scene resolver, the spotify wrapper, and
   `installMouseButtonHook` all use. Adding per-type entries is
   not novel architecture; it's the load-bearing pattern.
3. **Hot-reload-safe by construction.** Natives outlive chunk
   patches. The vm_patch path doesn't touch them. B2 would have
   to coordinate metadata re-registration with hot-reload; B1
   doesn't have to coordinate anything.
4. **Compact, debuggable bytecode.** One `OP_NATIVE_CALL` per
   drop site is easy to reason about in a trace. B3's unrolled
   sequences become a wall of `OP_GET_FIELD` + `OP_NATIVE_CALL`
   pairs that are hard to read and hard to compare against the
   C backend.
5. **Implementation effort matches what's already been done on
   the C side.** The synthesis logic in `c_backend.c` lines
   1183–1306 (`rae_drop_struct_<T>` + `_alias` emission) has a
   clear structural twin in `vm_compiler.c`. Cost is bounded.

The user's prior hypothesis was **B3** — uniform with normal VM
operations. The honest counter-argument: "uniform with normal VM
ops" is true for the *call sites* but not for the *cleanup
logic*. The cleanup logic is intrinsically per-type. B3 either
inlines it (verbose bytecode) or calls back into per-type natives
(B1 with extra steps). The bytecode-uniformity payoff doesn't
materialise.

A real point in B3's favour: **no native registry growth.** For a
large codebase with hundreds of cascade-droppable types, B1's
registration cost is non-trivial. For Rae's actual scale today
(mobile UI = ~60 types, royalblush-rae's projected size
similar), B1's registry footprint is small and fixed at module
load. If Rae ever lands a codebase where native count becomes a
real cost, B1's per-type natives can be folded into one
table-driven dispatcher without changing the public bytecode —
i.e. B1 can become a B2-ish implementation under the hood
without breaking any caller. **B1 doesn't paint into a corner.**

## Minimum change list if B1 is approved

Strictly the spike's scope is "pick"; the doc below sketches
what the implementation step would touch so the rough cost is
visible.

1. **Lift the classifiers.** Move `type_needs_cascade_drop`,
   `local_struct_owns_heap`, and `type_needs_deep_copy` from
   `compiler/src/c_stmt.c` to a new
   `compiler/src/ownership.{c,h}`. C backend updates its
   includes; nothing else changes.
2. **VM-side drop-helper synthesiser.** A new pass in
   `compiler/src/vm_compiler.c` that, for every cascade-
   droppable type, generates a small bytecode chunk (or native
   body) implementing the FULL and ALIAS cleanups. Registered
   via `vm_registry_register_native()`. Mirror of
   `c_backend.c:1183–1306`.
3. **Scope-exit / early-return emission.** In
   `compiler/src/vm_emit_stmt.c`, between expression evaluation
   and `OP_RETURN` (and at every block-scope close where locals
   are abandoned), emit `OP_GET_LOCAL <slot>` +
   `OP_NATIVE_CALL "rae_vm_drop_struct_<T>"` for each
   cascade-droppable local. Variant selection driven by the
   lifted classifier.
4. **Reassignment cascade.** Extend `OP_SET_LOCAL` /
   `OP_SET_FIELD` execution in `compiler/src/vm.c` to call the
   per-type cascade drop on the previous value (today
   `value_free` handles top-level only). Alternatively, emit the
   drop sequence explicitly at the compiler level before the
   set. Compiler-side is preferable; it's more in line with how
   the C backend works (drop emitted at compile time, not in the
   value system).
5. **Test 416-style stress probe.** Add a Live-mode case that
   loops over `List(String)` inside an `Outer` struct and asserts
   stable RSS, matching the C backend's behaviour.

Steps 3 and 4 are the bulk of the work. Step 1 is a refactor.
Step 2 mirrors known code. Step 5 is testing.

## What is explicitly NOT being decided here

* The exact FULL/ALIAS variant emitter (it'll port from
  `c_backend.c` mostly verbatim).
* Whether new opcodes are needed *eventually*. B1 doesn't
  require any. If profiling later shows the per-call overhead
  matters, a fused `OP_DROP_LOCAL` could be added without
  changing the surface design.
* The test numbering scheme. Defer to the Stage 1 plan doc.
* Whether to ship B1 in one PR or several. Implementation step
  will revisit.

## Summary recommendation

> **B1 — per-type native drop dispatcher, with the cascade-drop
> classifiers lifted to a backend-neutral header first.**
>
> B1 mirrors the proven C-backend structure 1:1, uses the
> VM's most-tested extension point (native registration),
> survives hot-reload without extra coordination, and yields
> compact, diffable bytecode. The user's B3 leaning was
> reasonable but doesn't pay off in practice: the per-type
> cleanup work has to live somewhere, and the bytecode-uniform
> appearance evaporates once recursion forces per-type helpers
> back into the design. B1 also leaves a clean upgrade path
> to a table-driven dispatcher if registry size ever becomes a
> real cost.

Next step (only when approved): implement steps 1–2 from the
"Minimum change list" above as a single commit, then steps 3–4
behind a feature flag with the Stage 1 test matrix gating
promotion.
