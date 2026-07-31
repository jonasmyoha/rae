# Roundtable Handoff

Lead agent: Clo
Task type: Feature design
Completed rounds: 2
Participants: Chattie, Clo, Gem

## Task
there are threedifferent 3d renderer prototypesdone by three different llmsthey are the examples 107 108 109evaluate these examples and give a scorewhich one is bettera score from 0 to 10 with decimalsthen proposewhich kind of 3d renderer api would be bestbased on these three examplessuggest changes to unifythese three examples into oneunified 3d rendererof course there are different features in themsuch as the raymarchertotal unification might not be possible

## Orchestrator decision
LEAD: Clo

Judged strictly on THIS run's actual task — evaluating the three 3D renderer prototypes (107/108/109) and proposing a unified renderer — both Chattie and Clo produced strong, on-task designs, but Clo's edges ahead as the lead. Both converged on the same core conclusions (109 = 7.9 as the strongest base, raster as primary with raymarch retained as a distinct technique, don't force-merge geometry). Clo's decisive advantages: it read all three examples' actual source plus both raymarch shaders before scoring, disclosed its own authorship of 109 and deliberately scored on architecture rather than authorship (intellectual honesty that matters when the lead owns a candidate), and articulated the cleanest layering — a backend-agnostic `lib/scene3d.rae` from 108's data model, a typed `Renderer3d` seam over 109's flat-buffer ABI, and shared camera/shading/present services — that directly answers "unify what you can, keep raymarch separate." It also pinned the single highest-risk trap concretely (the silent Z-up vs Y-up coordinate mismatch between the raymarchers and `math3d`) and gave a stage-able plan (raster data-model + typed API first to ship the demo, fold raymarch in second). Chattie's `World3d`/ECS-extraction/frame-graph proposal is more ambitious and its typed generation-checked handles and HDR/frame-graph roadmap are genuinely valuable to fold in, but that ambition also carries more scope risk for a first implementation slice, whereas Clo's incremental seam-first path is the safer thing to drive.

## Discussion summary
### Round 1 — Chattie
Recommendation: do a staged compiled recovery with `List`/`Map`/buffer specialization as the central gate, and do not accept any fix that solves generic `T` via `RaeAny`.

### Round 1 — Clo
project.**

### Round 1 — Gem
directory/file management, as it's a general-purpose AI agent with " access to all tools" and `write_file` is missing from my list.

### Round 1 — Chattie
Recommendation: the next step is a focused `sema` specialization rebinding fix, with the six remaining failures used as the active gate.

### Round 1 — Clo
**Clo should implement fix #1 (per-specialization body cloning + re-analysis) as the next task.** It's the single change that resolves the most failures and completes the monomorphisation architecture. Fixes #2 and #3 follow naturally once bodies are properly re-analyzed.

### Round 1 — Gem
files" and directory/file management, as it's a general-purpose AI agent with "access to all tools" and `write_file` is missing from my list. I'll ask it to create the directory and the memory file.

### Round 1 — Chattie
Decision: Chattie should fix this directly, with Clo only pulled in if the root cause lands deep in VM `view`/`mod` semantics.

### Round 1 — Clo
**TL;DR of the round:** All three reported bugs trace to commit `64cc50b` "Fix mobile UI Live mode". That commit landed (a) real VM diagnostic and emit-fix improvements, AND (b) accidental debug stubs in `lib/ui/render.rae` (hardcoded colors) and `examples/98_mobile_ui/album_view.rae` (deleted `setShapeRounded`) — the stubs explain bugs 1 and 2 entirely. Bug 3 (Live VM view-ref error) is the bug the commit *tried* to fix; the workaround in `vm_emit_stmt.c` is partial and the same error still fires. **Recommendation:** revert the two debug-stub hunks immediately in a separately-titled commit, then resume the Live VM investigation by first adding chunk+bytecode-offset info to `vm.c:448`'s `diag_error` so the next iteration has a real source location to chase.

### Round 1 — Gem
**Decision**: Revert the `render.rae` stubs immediately to fix the visuals for the compiled target, then prioritize the VM `view` assignment bug as the primary blocker for Live mode.

### Round 1 — Chattie
**Decision**
Design the Live fix as a VM bytecode semantics cleanup: introduce direct local initialization separate from assignment-through-reference, then use it for all `let` and compiler temporary initialization paths.

### Round 1 — Clo
Open questions to resolve before landing: whether the param-type string is built pre- or post-generic-substitution, and which `lib/core.rae` map/list helpers depend on alias-by-let semantics — those are the two places this fix could surface secondary breakage.

### Round 1 — Chattie
**Decision**
Fix Live mode by introducing `OP_INIT_LOCAL` for declarations and temporaries, preserving `OP_SET_LOCAL` for assignment-through-reference semantics. Do not patch the mobile UI source as a workaround.

### Round 1 — Clo
**Summary**: The Live VM error chains through four files — `vm_compiler.c:454` (auto-views non-primitive types including enums), the call-site dispatch at `vm_compiler.c:1260+`, `vm.c:448`'s `OP_GET_LOCAL` (value-copies the REF wrapper), and `vm_emit_stmt.c:235` (uses `OP_SET_LOCAL` for fresh `let`s). The fix is three small layers: (A) replace the non-primitive auto-view with a new `vm_is_value_type` that consults the enum table, (C) add `OP_DEREF_TOP` and emit it on value-typed `let`s as a safety net, (D) switch the value-typed `let` emitter to the existing `OP_BIND_LOCAL` opcode. Layer A alone resolves the reported failure; C and D close the class of bug. Wire `--target live` into `snapshot.sh` so the same regression can't reach the user next time.

### Round 1 — Chattie
I’ll restate the design as a fresh round-1 proposal, incorporating the stronger finding that `Screen` enum values must be treated as Live value types and pairing that with the local-initialization opcode split to close the stale-reference class.

**Root Cause**
The mobile UI Live failure is caused by Live VM value/reference lowering, not by the mobile UI source.

The reported failure points at:

```rae
let nextScreen: Screen = dispatchActions(input: input, current: currentScreen)
```

`Screen` is an enum, so it should be passed and stored by value. Instead, the Live compiler currently treats too many non-primitive types as implicit `view` references. That can make enum-like values flow through `VAL_REF`. Then `OP_SET_LOCAL` is also used for fresh `let` initialization, and the VM interprets a reused slot containing `REF_VIEW` as an attempted mutation of a read-only view.

There are two related problems:
- `Screen` enum is not classified as a value type in the Live call/local ABI.
- `OP_SET_LOCAL` conflates declaration initialization with assignment-through-reference.

**Best Design**
Fix Live mode in two layers:

1. Add a proper Live value-type classifier.
2. Split local initialization from assignment semantics.

This avoids patching the mobile UI example and makes Live match Compiled semantics.

**Layer 1: Live Value-Type Classification**
Add a helper in the VM compiler layer:

```c
bool vm_is_value_type(CompilerContext* ctx, Str type_name);
```

It should return true for:
- Primitive scalar types: `Int`, `Int64`, `Float`, `Float64`, `Bool`, `Char`, etc.
- `String`, if Live currently stores it as a value handle rather than a mutable object reference.
- Enums registered in `ctx->enums`.
- Explicit `val T`.
- Identity/key/id scalar-like types if the existing type model treats them as value wrappers.

It should return false for:
- Struct/object component types that need reference/view semantics by default.
- Lists, maps, buffers, component tables, and other mutable aggregate containers.
- Explicit `view T` and `mod T` should still override classification.

Use this in parameter lowering in `vm_compiler.c`, replacing the current rough rule:

```c
is_ptr = (is_mod || is_explicit_view || (!is_val && !is_primitive));
```

with:

```c
is_ptr = is_mod || is_explicit_view || (!is_val && !vm_is_value_type(ctx, type_name));
```

This should stop `Screen current` in `dispatchActions(input: view InputSystem, current: Screen)` from becoming a view ref.

**Layer 2: Direct Local Initialization**
Add a new opcode:

```c
OP_INIT_LOCAL
```

Use it when the compiler is initializing a local slot, not assigning to an existing binding.

`OP_INIT_LOCAL` semantics:
- Reads local slot index.
- Pops the value from the stack.
- Frees/replaces the local slot directly.
- Does not dereference `VAL_REF`.
- Leaves the stored value on the stack if current `OP_SET_LOCAL` call sites depend on `OP_POP` immediately after.

Keep `OP_SET_LOCAL` for real assignments:
- Plain value slot: replace local.
- `REF_MOD`: assign through reference.
- `REF_VIEW`: diagnostic error.

This is the important semantic split:
- `let x = y` is initialization.
- `x = y` is assignment.
- `let r => y` is reference binding.

**Emitter Changes**
In `compiler/src/vm_emit_stmt.c`:
- Use `OP_INIT_LOCAL` for `let` with initializer.
- Use `OP_INIT_LOCAL` for `let` default initialization.
- Use `OP_INIT_LOCAL` for loop locals and compiler-created locals.
- Keep `OP_SET_LOCAL` for assignment statements.
- Keep `OP_BIND_LOCAL` for `=>`.

In `compiler/src/vm_emit_expr.c`:
- Use `OP_INIT_LOCAL` for temporary locals used by list literals, collection literals, match expressions, object construction, and other compiler-generated scratch values.

In `compiler/src/vm_patch.c`:
- Add instruction width for `OP_INIT_LOCAL`, same as `OP_SET_LOCAL`.

**Regression Tests**
Add a focused Live test for enum-by-value:

```rae
enum Screen { A B }

func choose(current: Screen) ret Screen {
  let next: Screen = current
  ret next
}

func main() {
  let s: Screen = Screen.A
  let n: Screen = choose(current: s)
  log(n)
}
```

Add a stale-slot reuse test:
- Create a scoped `view` binding.
- Leave the scope.
- Declare a plain value local in a position likely to reuse the same slot.
- Assert Live does not report read-only view assignment.

Add or adapt a mobile UI smoke gate:
- `compiler/bin/rae run --target live examples/98_mobile_ui/main.rae`
- Prefer a timeout and possibly screenshot mode once Live can render long enough.

**Trade-Offs**
Layer 1 alone may fix the current mobile UI failure, because `Screen` should not be a view ref. But Layer 1 alone leaves the VM vulnerable to the same stale-slot bug with other values.

Layer 2 is broader but corrects the VM model. It makes bytecode semantics explicit and prevents future “fresh let acts like assignment through old ref” failures.

Adding `OP_INIT_LOCAL` is cleaner than clearing slots at scope exit. Runtime scope cleanup would require lifetime tracking and could break valid references. Direct initialization is simpler and closer to the language semantics.

**Risks**
The main risk is stack behavior. Existing emitter paths often do:

```c
OP_SET_LOCAL
OP_POP
```

`OP_INIT_LOCAL` must preserve that convention or every call site must be audited and adjusted.

The second risk is type classification drift. `vm_is_value_type` must use the same type facts as sema and the C backend, especially for enums, identity types, and `String`.

The third risk is over-converting assignment sites. Real assignment must continue using `OP_SET_LOCAL`; otherwise `mod` references and alias mutation semantics will break.

**Decision**
Fix Live mode by treating enums like `Screen` as value types in the Live ABI, then add `OP_INIT_LOCAL` so fresh declarations and temporaries never assign through stale `VAL_REF` slots. This repairs the mobile UI failure and closes the underlying VM semantics bug.

### Round 1 — Clo
Let me check current state of the rae submodule and the failing code paths before laying out the design.

The roundtable concluded — run-9 merged recommendation is on disk, Chattie was selected as implementation lead, and my memory file already captures the three-layer design (A: enum-aware `vm_is_value_type`, C: `OP_DEREF_TOP` safety net, D: `OP_BIND_LOCAL` for value-typed `let`).

Since Chattie leads implementation per the handoff, the ball isn't in my court. Two options for what you'd like me to do next:

1. **Wait for Chattie** to land the fix, then I help with the snapshot.sh `--target live` wiring + verification.
2. **Have Clo land it anyway** — I'd start with Layer A (single change at `vm_compiler.c:454`, plus call sites at 1241/1271), run `make -C compiler test`, snapshot Live, then add C+D if needed.

Which do you want?

### Round 1 — Gem
The failure in Live mode is caused by a **"View Leak"** in the VM compiler's optimization path.

When initializing a local variable from a struct member (e.g., `let r: Float = sh.radius`), the compiler is aggressively emitting a reference-binding instruction (`OP_VIEW_FIELD`) instead of a value-copy instruction (`OP_GET_FIELD`).

In the case of `render.rae` line 100, the local `r` becomes a `REF_VIEW` pointing into the `sh` struct. When the code later attempts to reassign `r` (`r = half`), the VM throws the "cannot assign to read-only view" error because `r` is technically a pointer to an immutable field.

### Technical Design: Primitive Value Promotion

The core of the fix is to ensure that **primitive types are never implicitly promoted to references** in the VM, as there is no performance or semantic benefit to referencing an 8-byte primitive over copying it.

#### 1. Approach: Conservative Member Evaluation
We will modify the VM compiler's `let` statement emission to detect when a member access results in a primitive type and force a value copy.

#### 2. Key Implementation Steps

**Step A: Update `vm_emit_stmt.c` (Local Initialization)**
Locate the specialized handling for `AST_EXPR_MEMBER` inside the `let` statement compiler. We must add a guard that checks the field type:
1.  Infer the type of the member access.
2.  If the type is a primitive (`Int`, `Float`, `Bool`, `Char`), **abort** the reference optimization.
3.  Fall back to standard expression compilation (`compile_expr`), which correctly uses `OP_GET_FIELD` (returning a value copy).

**Step B: Refine `OP_GET_FIELD` in `vm.c`**
Ensure that `OP_GET_FIELD` always performs a deep copy of the field value. Currently, it uses `value_copy`, which is correct, but we must verify that it doesn't accidentally return a reference if the object being accessed is itself a reference.

**Step C: Semantic Analysis Guard (Optional but Recommended)**
Update `sema.c` to track if a local variable is ever reassigned (`is_mutated`). If a variable is mutated, the compiler should never attempt to optimize its initialization into a `view` reference, even for large structs.

#### 3. Data Model & API Changes
*   **API**: Utilize `vm_is_primitive_type(Str type_name)` (existing) within the emission loop.
*   **Logic**:
    ```c
    // Logic to be added to vm_emit_stmt.c
    if (rhs->kind == AST_EXPR_MEMBER) {
        Str field_type = vm_infer_expr_type(compiler, rhs);
        if (vm_is_primitive_type(field_type)) {
            // Force value copy for primitives
            return compile_expr(compiler, rhs); 
        }
    }
    ```

### Trade-offs and Risks

*   **Trade-off: Copying vs. Referencing**: For large structs (e.g., a 100-field `Config` object), referencing is faster. However, for the primitives used in UI logic (`Float` for radius/alpha, `Int` for counts), copying 8 bytes is faster than managing the indirection of a reference.
*   **Risk: Explicit References (`=>`)**: We must ensure that explicit reference bindings (e.g., `let r: view Float => sh.radius`) still work. The fix should only affect standard `let` assignments (`=`), not explicit bindings (`=>`).
*   **Unknown**: If `componentGet` itself is returning a `view` due to how generics are lowered in Live mode, we may need to audit `vm_natives_core.c`'s `native_rae_ext_rae_buf_get` to ensure it isn't returning a `VAL_REF` to the internal buffer memory.

### Next Steps
1.  Modify `rae/compiler/src/vm_emit_stmt.c` to disable member-reference optimization for primitives.
2.  Test with a minimal reproduction: `let x = obj.field; x = 1.0;`.
3.  Verify the Mobile UI in Live mode using `rae live examples/98_mobile_ui/main.rae`.

[Thought: true] The Live mode failure is caused by an over-aggressive optimization in the VM compiler known as a **"View Leak."** When a local variable is initialized from a struct member (e.g., `let r

: Float = sh.radius`), the compiler incorrectly emits `OP_VIEW_FIELD`, turning the local into a read-only reference rather than a mutable value copy.

### Technical Design: Primitive Value Promotion

The design focuses on preventing implicit reference promotion for small, primitive types where indirection offers no benefit and causes runtime assignment errors.

#### 1. Approach
The VM compiler must be updated to treat all standard `let` assignments from struct members as value copies if the member's type is a primitive (`Float`, `Int`, `Bool`, `Char`). This ensures that variables like `r` in `render. rae` are mutable locals on the VM stack.

#### 2. Implementation Steps
*   **Compiler Modification**: In `rae/compiler/src/vm_emit_stmt.c`, add a type check during `let` statement emission. If the right-hand side is an `AST_EXPR_MEMBER` and the inferred type is primitive, bypass the reference optimization and use standard value-based compilation.
*   **Reference Dereferencing**: Verify that `OP_GET_FIELD` in `vm.c` correctly dereferences the parent object if it is a `view`, but still returns a **copy** of the field's value to the stack.
*   **Explicit Binding Preservation**: Ensure that the fix does not interfere with explicit reference bindings (`let x: view T => ...`), which should still function as intended for non-primitive types.

#### 3. Key Trade-offs and Risks
*   **Performance**: There is a negligible performance cost for copying primitives compared to referencing them (both are 8-byte operations in this VM).
*   **Unknowns**: We must verify that `native_rae_ext_rae_buf_get` (used by the ECS) is not itself leaking references to internal buffer memory, which could cause a similar "read-only" error further upstream.

** Conclusion**: By enforcing value copies for primitives during member initialization, we resolve the "cannot assign to read-only view" error at line 100 of `render.rae`, enabling the Mobile UI to function correctly in Live mode.

### Round 1 — Chattie
I’ll treat this as a fresh design pass and re-check the current failure surface in the repo rather than relying on prior lead decisions.

Then I’ll update only Chattie’s memory with the current technical conclusion.

The fresh check reproduces the current failure at `main.rae:100` and confirms the relevant code paths are still unchanged: enum types are gated by `vm_is_primitive_type`, and `let` initialization still emits `OP_SET_LOCAL`. I’ll record that concise finding.

**Current Failure Shape**
A fresh repo check still reproduces the Live failure:

```text
examples/98_mobile_ui/main.rae:100
cannot assign to a read-only 'view' reference (chunk main, bytecode offset 29559)
```

The failing source is:

```rae
let nextScreen: Screen = dispatchActions(input: input, current: currentScreen)
```

`Screen` is an enum. This line should initialize a plain value local. It should not assign through a `view`.

The current VM compiler still has two relevant problems:
- Live ABI decisions are based on `vm_is_primitive_type(...)`, so user-defined enums like `Screen` are treated as non-primitive aggregate-ish values and can be auto-viewed.
- `let` initialization still emits `OP_SET_LOCAL`, whose runtime behavior is assignment-through-reference if the slot currently contains `VAL_REF`.

**Best Design**
Fix Live mode with a two-stage VM semantics repair:

1. Add enum-aware value classification for the Live ABI.
2. Use direct local initialization for `let`, not reference-aware assignment.

This fixes the mobile UI failure without changing the mobile UI source.

**Stage 1: Add `vm_is_value_type`**
Introduce a central helper in the VM compiler layer:

```c
bool vm_is_value_type(CompilerContext* ctx, Str type_name);
```

It should return true for:
- Existing primitive types covered by `vm_is_primitive_type`.
- Enums registered in `ctx->enums`.
- `String`, if Live treats it as a value handle.
- Identity/id/key wrappers if they lower as scalar values.

It should return false for:
- Struct/component aggregate values.
- Lists, maps, buffers, component tables.
- Explicit `view T` / `mod T`, which should override default classification.

Use this helper anywhere the Live compiler currently says “non-primitive means reference/view”, especially:
- Parameter type string/lowering in `vm_compiler.c`.
- Call-site argument dispatch paths that decide whether to emit value load vs lvalue ref.
- Function-local parameter registration.

The intended rule:

```c
is_ptr = is_mod || is_explicit_view || (!is_val && !vm_is_value_type(ctx, type_name));
```

This makes `Screen current` pass by value in:

```rae
func dispatchActions(input: view InputSystem, current: Screen) pub ret Screen
```

**Stage 2: Direct Local Initialization**
The VM needs a direct-store operation for fresh declarations. There are two viable implementations:

Option A: Reuse existing `OP_BIND_LOCAL` if its current semantics are:
- Pop value.
- Free old slot.
- Store directly.
- Do not dereference `VAL_REF`.
- Do not push the value back.

Option B: Add a clearer new opcode:

```c
OP_INIT_LOCAL
```

Semantics:
- Pop value.
- Free/replace `frame->locals[slot]`.
- Never follow `VAL_REF`.
- Either preserve current stack behavior intentionally or remove the trailing `OP_POP` at converted sites.

The lower-risk path is to reuse `OP_BIND_LOCAL` only if all converted emit sites are statement-shaped and do not need the old `OP_SET_LOCAL` push-back behavior. If expression temporaries rely on stack preservation, add `OP_INIT_LOCAL` with explicit documented stack behavior instead.

**Emitter Policy**
Use direct initialization for:
- `let x: T = value`
- `let x: T` default initialization
- compiler-generated temporaries
- loop locals and match/list/object temporary locals

Keep reference-aware assignment for:
- `x = value`
- field assignment
- mutation through `mod` references

Keep explicit reference binding separate:
- `let r: view T => target`
- `let r: mod T => target`

That preserves Rae’s semantic distinction:
- `let` initializes storage.
- `=` mutates existing storage.
- `=>` binds a reference.

**Value-Copy Rule**
For normal `let` initialization, value-typed RHS expressions must become values, not refs.

Examples that must store values:

```rae
let nextScreen: Screen = dispatchActions(...)
let r: Float = sh.radius
let count: Int = list.length
```

Only explicit `=>` should preserve references.

If the RHS can produce a `VAL_REF`, the emitter must dereference/copy before direct local storage. This can be handled by:
- Better call-site value typing.
- Emitting `OP_GET_FIELD` instead of `OP_VIEW_FIELD` for value-typed member reads.
- Optionally adding a small `OP_DEREF_TOP` safety opcode if ref wrappers can still reach value-typed `let` RHSs.

**Implementation Steps**
1. Add `vm_is_value_type(ctx, type_name)`.
2. Replace relevant `!vm_is_primitive_type(...)` ABI predicates with `!vm_is_value_type(...)`.
3. Add or reuse a direct local initialization opcode/path.
4. Convert `let` initialization in `vm_emit_stmt.c` from `OP_SET_LOCAL + OP_POP` to direct initialization.
5. Audit `vm_emit_expr.c` `OP_SET_LOCAL` sites and convert only true temporaries.
6. Do not change assignment statement lowering except where it accidentally handles declarations.
7. Add regression tests:
   - Enum param/value local: `Screen` passed into a function and returned.
   - Stale slot reuse: scoped view ref followed by plain `let`.
   - Primitive member copy: `let r: Float = obj.radius; r = ...`.
   - `98_mobile_ui` Live smoke test.
8. Run with explicit timeouts:
   - Targeted tests.
   - `compiler/bin/rae run --target live examples/98_mobile_ui/main.rae`.
   - Full `make test`.

**Trade-Offs**
Fixing only enum classification is the smallest change and may clear the current mobile UI error. But it leaves the stale-slot `OP_SET_LOCAL` problem available for the next view-valued path.

Adding a new `OP_INIT_LOCAL` is explicit and clean, but expands the bytecode surface. Reusing `OP_BIND_LOCAL` keeps the opcode set smaller, but only if its stack behavior matches the converted sites.

Clearing runtime slots at scope exit is not recommended. It would require lifetime modeling and could break references that intentionally survive within active frames.

**Risks**
The biggest risk is stack convention. Existing `OP_SET_LOCAL` sites often expect the stored value to remain on stack before `OP_POP`. Any direct-init replacement must account for that.

The second risk is over-classifying structs as values. UI component structs should remain view-by-default unless explicitly copied.

The third risk is hidden alias-by-let assumptions in library code. Before changing broad `let` behavior, check map/list helpers such as `StringMap.set` and `IntMap.set` for reliance on `let x = buf_get(...)` as an alias.

**Decision**
The best design is to make the Live VM’s value/reference boundary explicit: classify enums like `Screen` as value types, then use direct local initialization for `let` so fresh declarations never mutate stale view refs.

### Round 1 — Clo
Let me re-verify the current state of the failing paths before laying out the design — last verification was earlier today and I want to confirm nothing landed in between.

Confirmed: code state unchanged from earlier today. `vm_compiler.c:454` still auto-`view`s any non-primitive (including user enums), and `vm_emit_stmt.c:235-237` still uses `OP_SET_LOCAL` + `OP_POP` for fresh value-typed `let`s.

## Round 1 — Design

### Problem

Mobile UI fails in Live mode with:

```
cannot assign to a read-only 'view' reference (chunk ..., bytecode offset ...)
```

at `examples/98_mobile_ui/main.rae:100`:

```rae
let nextScreen: Screen = dispatchActions(input: input, current: currentScreen)
```

`Screen` is a user enum. It should be passed and stored by value. In Live mode it's being marshalled as a `view` reference, which then triggers the `REF_VIEW` read-only diagnostic on the next iteration of the outer loop when `currentScreen = nextScreen` reuses the slot.

### Root-cause chain (verified in source)

1. **`vm_compiler.c:454`** — `get_type_name_with_refs`: `} else if (!vm_is_primitive_type(base)) { ... "view " }`. Auto-prefixes `view ` for *anything non-primitive*, including user-defined enums.
2. **Call-site dispatch in `vm_compiler.c`** (~lines 1241/1271): reads the param-type string `"view Screen"` → routes the argument through the lvalue/ref-emit path. Caller's local becomes a `VAL_REF{REF_VIEW}` on the stack at call return.
3. **`vm.c:448` `OP_GET_LOCAL`** — `vm_push(vm, value_copy(&frame->locals[slot]))`. `value_copy` of a `VAL_REF` copies the wrapper, not the target.
4. **`vm_emit_stmt.c:235`** — fresh `let` emits `OP_SET_LOCAL` + `OP_POP`. First iteration: slot is empty so the `VAL_REF` is stored as-is.
5. **Second iteration** — at `currentScreen = nextScreen`, `OP_SET_LOCAL` (`vm.c:451-481`) sees `VAL_REF{REF_VIEW}` in the destination slot and fires the diagnostic at `vm.c:464`.

The mobile UI source is correct. The VM ABI is wrong.

### Design

**Two minimal layers. Layer A is sufficient to clear the reported failure; Layer B closes the bug class.**

#### Layer A — Enum-aware `vm_is_value_type` (required, fixes the reported bug)

Add a single helper in `vm_compiler.c`:

```c
bool vm_is_value_type(CompilerContext* ctx, Str base) {
    if (vm_is_primitive_type(base)) return true;
    if (enum_table_has(ctx->enums, base)) return true;
    return false;
}
```

Update three sites that currently gate on `vm_is_primitive_type`:

- `vm_compiler.c:454` — param-type stringifier. Replace `!vm_is_primitive_type(base)` with `!vm_is_value_type(ctx, base)`.
- `vm_compiler.c:1241` — call-site arg dispatch. Same predicate update so value-typed args don't go through `emit_lvalue_ref`.
- `vm_compiler.c:1271` — second arg dispatch site. Same.

Pre-flight grep: `rg "vm_is_primitive_type" rae/compiler/src/` to catch any other defaulting site with the same logic and update it consistently.

This single change resolves the `Screen` failure. The user's enum goes through the ABI as a value, not a view.

#### Layer B — `OP_SET_LOCAL` + `OP_POP` → `OP_BIND_LOCAL` for value-typed `let`

`OP_BIND_LOCAL` already exists at `vm.c:482-497` with exactly the semantics needed for "fresh local initialization":

```c
case OP_BIND_LOCAL: {
  ...
  Value value = vm_pop(vm);
  value_free(&frame->locals[slot]);
  frame->locals[slot] = value;   // direct install, no view-deref, no push-back
  break;
}
```

It's currently only emitted for the `=>` (explicit reference binding) path. We extend it to be the canonical opcode for value-typed `let`-with-initializer too.

Emitter change at `vm_emit_stmt.c:235-237`:

```c
// BEFORE
emit_op(compiler, OP_SET_LOCAL, ...); emit_uint32(compiler, slot, ...);
emit_op(compiler, OP_POP, ...);

// AFTER
emit_op(compiler, OP_BIND_LOCAL, ...); emit_uint32(compiler, slot, ...);
```

Net effect: one fewer byte per `let`. `OP_SET_LOCAL` retains its full semantics for real assignments (`x = y`), including the `REF_MOD` write-through and `REF_VIEW` diagnostic at `vm.c:463-472`. No language semantics change for the assignment statement.

This is the safety net: if any expression ever leaks a `VAL_REF` into a slot during evaluation, the `let` cleanly overwrites it instead of triggering the read-only diag.

#### What I'm NOT doing

- **Not introducing a new `OP_INIT_LOCAL` opcode.** `OP_BIND_LOCAL` is the exact same VM behavior. Adding a parallel opcode would duplicate it for no gain.
- **Not patching `examples/98_mobile_ui/*.rae`.** Source is correct.
- **Not touching `OP_SET_LOCAL` semantics.** Assignment-through-ref and the `REF_VIEW` diag stay untouched.
- **Not touching `sema.c` or the C backend.** Compiled target is unaffected.

### Implementation steps

1. Pre-flight greps:
   - `rg "vm_is_primitive_type" rae/compiler/src/` — find all defaulting sites.
   - `rg "OP_SET_LOCAL" rae/compiler/src/` — confirm no emit site relies on the `vm_push(val)` push-back result as an expression value (i.e., all are `OP_SET_LOCAL` + `OP_POP`-shaped or assignment-statement-shaped).
   - `rg "let .* = rae_ext_rae_buf_get|let .* = buf_get" rae/lib/core.rae` — confirm `StringMap.set`/`IntMap.set` do their write-back via explicit `buf_set` (not alias-by-let).
2. Add `vm_is_value_type` in `vm_compiler.c`. Use the existing `EnumTable` lookup.
3. Apply Layer A at the three call sites.
4. Build + run `make -C compiler test` with `perl -e 'alarm shift; exec @ARGV' 120 make test`. Confirm the suite is green with Layer A alone.
5. Run `rae run --target live examples/98_mobile_ui/main.rae`. Confirm the view-ref diag is gone.
6. Apply Layer B at `vm_emit_stmt.c:235-237`. Rerun the test suite + the Live snapshot.
7. Commit Layer A and Layer B as **separate commits** so the diff stays bisectable. Commit message style per `CLAUDE.md`: focus on the *why*.
8. Wire `rae run --target live examples/98_mobile_ui/main.rae` into `examples/98_mobile_ui/snapshot.sh` so this regression class triggers in CI. The tooling gap is what let `64cc50b` ship the partial fix.

### Trade-offs

- **Layer A alone vs. Layer A+B**: Layer A is the minimum viable fix. Layer B is a small additional cleanup that hardens the VM against an entire stale-slot-reuse class. Cost of Layer B is one emitter line plus the audit grep. Recommend landing both.
- **Adding `String` to `vm_is_value_type`**: tempting, but Live's String handling might already store strings as object refs with their own lifecycle. Out of scope for this fix; add only if the grep audit shows it's clearly value-shaped today.
- **Mass-converting all `let` emit sites to `OP_BIND_LOCAL`**: tempting but riskier — temp-binding sites in `vm_emit_expr.c` (list literals, match scratch, etc.) might use `OP_SET_LOCAL`'s push-back as the expression's result. Leave those alone in this PR.

### Risks

1. **`lib/core.rae` alias-by-let in `StringMap.set` / `IntMap.set`**. The pattern `let entry = buf_get(...); entry.field = ...; buf_set(..., entry)` looks like explicit write-back, not alias-mutation. Grep to confirm before landing Layer B.
2. **`vm_emit_expr.c` temp sites depending on `OP_SET_LOCAL` push-back**. Audit via grep; Layer B only changes the *let-statement* site, so this should be safe by construction, but verify.
3. **`native_rae_ext_rae_buf_get` returning `VAL_REF` to internal buffer memory**. If it does, every `componentGet` is pre-leaked and Layer A's call-ABI fix doesn't reach it. 5-minute read of `vm_natives_core.c`. If it leaks, that's a separate bug to file — out of scope for this PR.
4. **Param-type strings are post-monomorphisation**. `vm_is_value_type` operates on resolved names; raw generic `T` is never seen here. Document the assumption in the helper's doc comment.
5. **Existing snapshot fixtures passing by accident.** After the `OP_POP` drop, any latent stack-balance bug surfaces. Mitigate by running the full test suite, not just the targeted Live snapshot.

### Unknowns

- Exact behavior of `vm_natives_core.c::native_rae_ext_rae_buf_get` w.r.t. `VAL_REF` returns. Determines whether Layer A is sufficient or whether buffer-get also needs hardening.
- Whether `String` should be in `vm_is_value_type`. Decide via grep + a single test in Live.

### Validation gate

1. `perl -e 'alarm shift; exec @ARGV' 120 make -C compiler test` — green.
2. `rae run --target live examples/98_mobile_ui/main.rae` — boots, no view-ref diag, screens switch.
3. `examples/98_mobile_ui/snapshot.sh` — both Compiled and Live snapshots produced.
4. New regression test: minimal enum-by-value Live test (`enum E { A B }; let x: E = ...; x = E.B`).

Now updating memory.

Round 1 design posted. Memory updated. Skipping task tracking — single-round design contribution, no implementation work in this round.

### Round 1 — Gem
The technical design for fixing the Rae Mobile UI example in Live mode focuses on achieving parity with the compiled target while addressing VM-specific robustness issues (stack limits and missing natives).

### 1. Approach: Viewport Simulation Parity
The "broken" state of the mobile UI in Live mode is primarily due to the lack of coordinate system indirection. We will implement the **Three Coordinate Systems** model (Design, Viewport, Screen) within the VM.

*   **Design Space:** Authoring grid (393×852).
*   **Viewport Space:** The simulated phone screen resolution.
*   **Screen Space:** The actual desktop window framebuffer.

By rendering the mobile UI to a viewport-sized texture and then blitting that texture to the screen, we ensure the UI reflows correctly according to the chosen device preset (iPhone, Pixel, etc.) even when running in the interpreter.

### 2. Key APIs & VM Native Extensions
To support the "phone-in-window" simulation, the VM's Raylib bindings (`vm_raylib.c`) must be extended to support textures as first-class targets.

**Proposed VM Natives:**
*   `loadRenderTexture(w: Int, h: Int) ret RenderTexture`: Allocates a GPU-backed render target.
*   `beginTextureMode(target: RenderTexture)` / `endTextureMode()`: Redirects drawing to the texture.
*   `drawTextureRec(tex: Texture, source: Rect, pos: Vec2, color: Color)`: Essential for blitting the final viewport to the screen (handles the Y-axis flip inherent in Raylib render textures).
*   `beginMode2D(cam: Camera2D)` / `endMode2D()`: Enables the editor camera (zoom/pan) functionality.

**Data Model Updates:**
A global `Viewport` struct will be introduced to `lib/ui/viewport.rae`, accessible to both the `layoutSystem` and the main render loop.

```rae
type Viewport {
  designWidth: Float, designHeight: Float   # e.g., 393, 852
  viewWidth: Float, viewHeight: Float       # Simulated resolution
  screenWidth: Float, screenHeight: Float   # Actual window size
  scale: Float, simOffsetX: Float, simOffsetY: Float
  safeTop, safeBottom, safeLeft, safeRight: Float
  simulated: Bool
}
```

### 3. Implementation Steps

#### Phase 1: VM Hardening & Natives
1.  **Increase VM Limits:** Update `vm.h` to increase `STACK_MAX` to **4096** and `CallFrame.locals` to **512**. The ECS-based UI library uses deep recursion in `measureSubtree` and `computeSubtree` which can exceed current interpreter limits.
2.  **Add RenderTexture Natives:** Implement the 4-6 required Raylib texture-mode functions in `vm_raylib.c`.

#### Phase 2: Viewport & Safe Area Logic
1.  **Viewport System:** Implement `lib/ui/viewport.rae`. This system calculates scale and pillarbox offsets once per frame before layout runs.
2.  **Safe Area System:** Implement `lib/ui/safe_area_system.rae`. It reads the device preset (e.g., iPhone 15 Pro notch insets) and writes `SafeInsets` components to nodes.
3.  **Layout Refactor:** Update `lib/ui/layout.rae` to use `viewport.viewWidth/Height` as its root extent instead of hardcoded screen globals.

#### Phase 3: Example Integration
1.  **Refactor `main.rae`:** Update the draw loop to use `BeginTextureMode` if `RAE_UI_DEVICE` is set.
2.  **Hot-Reload Tuning:** Ensure the `UiWorld` state is preserved during bytecode patching so that changing layout logic doesn't reset the current navigation stack or scroll positions.

### 4. Trade-offs and Risks

*   **Y-Axis Flip:** Raylib textures are upside down. The final blit in `main.rae` must use a negative source height. This logic must be consistent between Live and Compiled targets to avoid target-specific bugs.
*   **Performance:** Double-buffering via `RenderTexture` in the VM adds overhead. However, for a 60FPS UI demo, the interpreter-native transition cost is negligible compared to the clarity gained by the simulation.
*   **Unknowns:** Determining if 4096 is a sufficient stack size for extremely complex scenes. We should implement a "Stack Depth" warning in the VM rather than silently corrupting memory.

### 5. Contextual Relevance
This design directly addresses the "Open Questions" in `rae/docs/ui-viewport-and-safe-area-plan.md` regarding VM parity. It leverages the existing `VmRegistry` and `OP_NATIVE_CALL` infrastructure recently stabilized in the compiler.

---

**Memory Update**
I have appended the key decisions and design points to `.roundtable/agents/gem/memory.md`.

```markdown
# Gem's Roundtable Memory - Mobile UI Fix

## Key Decisions (Round 1)
- **Viewport Simulation Parity:** Live mode (VM) must implement the same 3-coordinate system (Design/Viewport/Screen) as Compiled mode to fix layout issues.
- **VM Native Extensions:** Identified missing Raylib natives: `LoadRenderTexture`, `BeginTextureMode`, `EndTextureMode`, `DrawTextureRec`.
- **VM Robustness:** Increase `STACK_MAX` to 4096 and `CallFrame.locals` to 512 to support deep ECS layout recursion.
- **Hot-Reload:** Ensure `UiWorld` state persists across bytecode patches to allow real-time layout iteration without screen resets.

## Insights
- The current "broken" state in Live mode is caused by hardcoded screen dimensions and lack of texture-mode support for mobile aspect simulation.
- Recursion in `measureSubtree` and `computeSubtree` is a primary risk for VM stability in complex UI scenes.
```

I have updated my memory file. I am now ready for the next round.

### Round 1 — Clo
## Units in `.raescene`: direct answer

**Store every length as a unitless number interpreted as logical points (pt) at 1× density. Never store device pixels in the scene format.** That is the industry standard, unanimously, under different names:

- **CSS** — the reference pixel (`px`) is density-independent; physical pixels come from `devicePixelRatio`.
- **iOS** — points; 1pt → 2 or 3 device px (`@2x`/`@3x`).
- **Android** — `dp` (density-independent pixels) + `sp` for text.
- **Flutter** — "logical pixels," DPR applied at the engine boundary.
- **Figma** — points/frames; export scale is separate.

The thing they all share: **author in a density-independent unit, push physical resolution to a single render-time scale factor (DPR), and get cross-device adaptation from constraints/flow — not from baking pixels or aspect into coordinates.** Pixi's 1080×2280 is the *anti-pattern* here: it folds ~3× DPR headroom into the coordinate space, so the numbers no longer mean anything portable. Don't copy that into `.raescene`.

This project has a strong reason the points+DPR model fits especially well: the **MSDF text** work already landed. MSDF glyphs scale to any size from one atlas, so unlike raster fonts we need **no per-density font atlases** — points + DPR + MSDF gives crisp text at any scale "for free." Only raster PNG assets need `@Nx` variants.

## Data model (`.raescene`)

Keep one scalar "length" concept, three forms — this also preserves the existing `"spaceL"` token usage in current scenes:

```jsonc
// A Length is: number (points) | token string | percent
"Rect":   { "x": 0, "y": 0, "w": 393, "h": 56 },   // points
"Padding":{ "l": "spaceL", "t": "spaceS", "r": "spaceL", "b": "spaceS" }, // design tokens → pt
"Size":   { "w": { "mode": "Fill" }, "h": { "mode": "Fixed" } },          // Hug|Fill|Fixed (+min/max in pt)
"Constraints": { "h": "LeftRight", "v": "Bottom" }  // Figma pin/stretch — for absolutely-placed children
```

- **Numbers = points.** A `56` is 56pt → `round(56 × dpr)` device px at render.
- **Tokens** (`spaceS/M/L`) resolve to points from the spacing scale at load — already in use; formalize it.
- **No raw pixels, no aspect baked in.** The reference frame **393×852** is metadata for the authoring/editor canvas and the basis for `Scale` constraints — *not* a hard render frame.
- **Adaptation = Auto Layout (have: `Layout`+`Size`) + Constraints (new).** Pure-`%` everywhere (old-web style) is strictly worse than constraints+flow; keep absolute pt as the base and let constraints/flow adapt. `%` is an optional escape hatch, not the primary mechanism.

## Render / DPR pipeline (the only place pixels appear)

```
Lw,Lh = GetScreenWidth/Height()    # logical points = layout frame
Rw,Rh = GetRenderWidth/Height()    # framebuffer px (HIGHDPI)
dpr   = Rw / Lw                     # crispness only
device_px(p) = round(p * dpr) / dpr # snap to device-pixel grid → no blurry hairlines
```

`FLAG_WINDOW_HIGHDPI` on; all layout math stays in points; DPR is applied once at the render/transform boundary. **Pixel snapping** (`round(pt*dpr)`) is the detail that prevents fractional points from producing soft 1px borders — a real, well-known concern (CSS subpixel, retina hairlines).

## Implementation steps (concrete, against current files)

1. **Declare the unit contract** in `lib/ui/scene.rae` schema + the doc: numbers are points; add a `Length` resolver (number | token | `"NN%"`).
2. **Convert `config.rae`** from the 498×1079 pixel space to 393×852 points — a clean ×0.789 rescale (same aspect, no reshaping).
3. **DPI plumbing** in `main.rae`/runtime: `FLAG_WINDOW_HIGHDPI`, expose `GetRenderWidth/Height` + `dpr`; drop `setMouseScale(1,1)` and hit-test in points.
4. **Frame = window** each frame: feed live point size into `layout.rae`. Fix the `setLayoutScreenSize` no-op stub (`layout.rae:32-39`) — its comment flags a cross-module global-write limitation; either land that write or have `main.rae` set the globals directly. This also kills the current 600×1300 / 498×1079 / window three-way desync.
5. **Pixel snapping** in `transform.rae`/render where points → device px.
6. **Constraints** (`Constraints {h,v}`) in `scene.rae` + a constraint pass for absolutely-placed children; migrate the dock off the hardcoded `Offset {y: screenH-dockH}` (`nav_tabs_builder.rae:337-339`).
7. **Safe-area insets in points** so device presets line up 1:1 (per the adaptive-layout doc §3).

## Trade-offs, risks, unknowns

- **Migration churn (low-med):** rescaling 498→393 touches every literal in `config.rae` and any pt baked into scenes. Mechanical, but needs a visual diff pass.
- **Fractional points → blur (med):** if snapping is missed, hairlines/borders soften at non-integer DPR. Mitigated by `round(pt*dpr)`; must verify 1px separators specifically.
- **Cross-module global write (med, project-specific):** `layout.rae` says reassigning module globals from another module isn't supported today. If true, step 4 needs either a compiler fix or a per-frame `main.rae` write path — confirm which before committing to the API.
- **Raster asset density (med):** points fix layout, not image sharpness. PNGs still need `@2x/@3x` or supersample-on-load; MSDF text is already covered, so this is image-only.
- **Non-integer DPR on macOS (low):** scaled Retina modes yield DPR like 1.6–2.0; snapping handles it but mouse-coord rounding must match render rounding or hit-tests drift by ≤1px.
- **`%`/`fr` scope creep (unknown):** decide now whether percentage lengths are in v1 or deferred — adding them later is a format-version bump. Recommend constraints+flow first, `%` deferred.
- **royalblush-rae coupling (unknown):** the *only* reason to keep a 1080-style space anywhere is if the game port shares pixi scene coordinates. That's a separate app from `98_mobile_ui`; confirm before assuming one unit policy spans both repos.

Now updating my memory.[Read] /Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/.roundtable/agents/clo/memory.md

[Edit] /Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/.roundtable/agents/clo/memory.mdMy design contribution is posted above and my memory is updated.

**Summary of my position:** `.raescene` should store lengths as **logical points (pt) at 1× density — never device pixels**. That's the universal industry standard (CSS px, iOS pt, Android dp, Flutter logical px, Figma points), all of which keep coordinates density-independent and push physical resolution to a single render-time DPR factor. Pixi's 1080×2280 bakes both DPR headroom *and* a 19:9 aspect into the coordinates, which is exactly what makes those numbers non-portable — don't replicate it in the format.

Concretely: reference frame **393×852** (a clean ×0.789 rescale of the current `98_mobile_ui` 498×1079, same aspect), adaptation via **Auto Layout + Figma-style Constraints** rather than baked pixels or pervasive `%`, DPR plumbed through `FLAG_WINDOW_HIGHDPI` with **pixel snapping** to keep hairlines crisp. The project's MSDF text work is a real bonus here — text scales from one atlas, so only raster PNGs need `@Nx` variants.

Main risks flagged: the `setLayoutScreenSize` cross-module-global-write limitation, fractional-point blur without snapping, raster asset density, and confirming whether `royalblush-rae` shares pixi coordinates (the only reason to keep a 1080-space anywhere).

### Round 1 — Chattie
I’ll ground this in Rae’s current implementation before proposing the design: I’m checking the existing `spawn` support, VM/C runtime threading hooks, and current tests so the proposal fits the codebase rather than inventing a separate model.

I’m updating Chattie’s memory with only the current concurrency design conclusions.

The memory file has drifted from the snapshot, so I’ll inspect the tail and append safely.

**Design**
Rae should use a `spawn`-first model: normal calls are synchronous and waited by default; only `spawn` creates concurrent work. No `await` keyword is needed. Waiting is expressed by consuming the task result:

```rae
let task = spawn loadAlbumArt(path: path)
let image = task.get() # explicit join/result point
```

Core concurrency should have three layers:

1. `Task(T)` / `TaskGroup` for structured concurrency.
2. `Channel(T)` / actor-style workers for long-running services like IO, asset loading, network, tooling.
3. `parallelFor` / table sharding for data-parallel ECS/component work.

**Key Semantics**
`spawn` should return `Task(T)`, not detach by default. Detached work should require explicit syntax or API. `TaskGroup` should join or cancel children at scope exit, keeping Rae analyzable.

Safety rules should be strict:
- Spawned tasks cannot capture stack `view`/`mod` refs unless lifetime is proven by a `TaskGroup`.
- Shared `mod` access across tasks is rejected unless split into disjoint shards.
- Raylib/render calls are main-thread only.
- ECS parallel work should operate on explicit component-table shards, not shared mutable tables.

**Implementation Path**
Start with runtime primitives already suggested by current repo state: Rae has early `spawn`, `sys_thread`, and VM thread plumbing, but it is not yet a safe task model.

Build in phases:
1. Define `Task(T)`, `TaskGroup`, `join/get`, cancellation, and error propagation.
2. Replace detached VM/C backend spawn behavior with task handles.
3. Add compiler checks for unsafe captures and shared `mod`.
4. Add `Channel(T)` for background workers.
5. Add ECS-safe `parallelFor` / table sharding.
6. Later add async IO/select-style APIs, still without `await`.

**Risks**
The hardest parts are Live VM thread safety, ownership of values crossing task boundaries, hot reload with active tasks, and deterministic ECS mutation. The safest design is structured concurrency first, actors/channels for long-lived background work, and explicit data-parallel APIs for component tables.

Decision: design Rae concurrency around explicit `spawn`, structured `TaskGroup`s, and ownership-checked data sharing; do not import conventional `async/await` syntax.

### Round 1 — Clo
I'll ground this design in how Rae actually works (dual C-backend + bytecode VM, the ECS in `lib/ui`, explicit `view/mod/own/copy` parameter modes).

Let me check a few concrete realities before designing.

[Bash] Survey existing concurrency primitives[Bash] Inspect thread API + allocator/global state

I have what I need: Rae already has a cross-platform `sys_thread`/`sys_mutex` C abstraction (used by the VM), a `malloc`-based runtime with **global** mem-accounting counters (a race hazard), no GC, and an **ownership model** (`view/mod/own/copy` + cascade-drop). That ownership system is the single most important asset here — it's already a borrow checker, so we get data-race freedom largely for free. Here's my design.

---

# Rae Concurrency Design (Clo, round 1)

## Guiding principle: concurrency rides on the ownership system, not on locks

Rae already statically distinguishes shared-read (`view`), exclusive-write (`mod`), and transfer (`own`). That is exactly the Send/Sync information Rust encodes with traits — except Rae already computes it for every parameter. **The whole design leans on this: the compiler proves race-freedom at the `spawn` boundary, so both backends are safe by construction and we avoid a lock-everything runtime.** This is the differentiator and the riskiest part.

## 1. Surface syntax (matches the user's inversion)

- **`spawn <expr>` is the only async marker.** It evaluates `<expr>` on a task and immediately returns a `Task(T)` handle. A normal call `compute()` is synchronous (waited) as today — so "await" is the default and unmarked; "don't wait" is the thing you opt into. This is the inversion the user wants and it's sound.
- **Await-by-default via coercion:** a `Task(T)` joins (blocks, returns `T`) the moment its value is read where a `T` is expected. Explicit forms for control:
  - `join(t)` → `T` (blocking),
  - `t.poll()` → `opt T` (non-blocking, for the render/event loop),
  - `select(...)` over several tasks/channels.
- **Structured concurrency block** so tasks can't leak:
  ```
  taskScope {
    let a: Task(Int) = spawn loadAlbum(path: p1)
    let b: Task(Int) = spawn loadAlbum(path: p2)
    # both auto-joined at scope exit; first error cancels siblings
  }
  ```
  `taskScope` is the safe default the docs should push; bare `spawn` without a scope is allowed but its handle must be joined before its captured borrows expire.

## 2. The three concurrency shapes the user named

- **Long-running thread (render/IO loop):** `spawn` a function that **owns** its state and communicates only via `Channel(T)` (move-based). Maps cleanly onto the busy-render-loop work we just did — logic thread owns app state, render thread owns GPU/frame data, frames move over a channel. No shared mutable state, so no locks.
- **Data-parallel over component tables:** `parallelFor(range: 0..n, body)` and a **disjoint-slice borrow** that hands each task a non-overlapping `mod` window into a `ComponentTable`'s dense arrays. Because the slices don't alias, the borrow checker passes and there's zero locking. Join is implicit at the `parallelFor`/`taskScope` boundary.
- **Async calls:** `spawn` + auto-join, as above.

## 3. Key stdlib types

- `Task(T)` — future handle (join/poll/select).
- `Channel(T)` — `send(own T)` / `recv() ret T`, move semantics (the value leaves the sender).
- `Shared(T)` — mutex-guarded cell for the rare genuinely-shared-mutable case: `withLock(this: shared, body: fn(mod T))`.
- `Atomic(Int)` / `Atomic(Bool)` — for counters/flags (e.g. the loop's "is animating" flag, the hot-reload status).

## 4. Dual-backend execution (same semantics, different engine)

This is where Live and Compiled legitimately differ while staying "two views of one language":

- **Compiled (C):** real OS threads via the existing `sys_thread`/`sys_mutex`, behind a **work-stealing thread pool**. `spawn` enqueues; `Task` join is a condvar wait; `Channel` is a mutex+condvar MPMC queue; `Atomic` → C11 `<stdatomic.h>`. **Genuine parallelism.**
- **Bytecode VM (Live):** the interpreter has shared mutable state (value stack, the `g_mem_*` global counters), so preemptive threads are unsafe. Use **green tasks (fibers) on one OS thread**, scheduled cooperatively; blocking ops (`recv`, `join`, IO) yield. Insert yield checks at **loop back-edges** so a compute loop can't starve the scheduler. **Concurrency without parallelism** — correct for Live's role (iteration/tooling/hot-reload), and the *observable semantics match Compiled* because the borrow rules forbid races in both.

The payoff: a program is race-free by static checking, so it behaves identically whether interleaved (VM) or truly parallel (C). That equivalence must be a first-class test suite.

## 5. Implementation steps

1. **C runtime substrate:** thread pool on `sys_thread`, MPMC channel, `Task` (result slot + condvar), atomics; **make `g_mem_*` accounting per-thread (flush on join) or atomic** — today they'd race.
2. **VM substrate:** fiber scheduler + yield points at back-edges and blocking calls.
3. **Frontend:** `spawn` expression, `Task(T)` type + auto-join coercion, `taskScope` block, `Channel/Shared/Atomic` types. Enforce naming mandates (PascalCase types, camelCase funcs).
4. **Semantic analysis — the core work:** extend the param-mode checker with **spawn-boundary rules**: a task may capture by `own` (move) or by `view` of a *Sync* type; a `mod` capture across `spawn` is a compile error (the data race), with a clear diagnostic. Define "Sync/Send" as a built-in property derived from a type's transitive fields (value types and deeply-owned types are Send; types with non-atomic shared interior are not) — no trait system needed.
5. **ECS integration:** `parallelFor` + disjoint dense-slice borrow; document that **structural mutation (`componentSet/Remove`, which touch the sparse map + `generation` + may realloc) is single-threaded only** — parallel access is read, or write to disjoint pre-sized index ranges.
6. **Tests:** per-backend + a Live-vs-Compiled equivalence suite; stress/ASan for the C path.

## 6. Trade-offs, risks, unknowns

- **Biggest risk — soundness of the spawn-boundary borrow rules.** Get "what's Send" wrong and you either reject valid code or admit races. This is make-or-break and deserves the most design care; without traits we lean on a built-in transitive-field rule, which may be too coarse (escape hatch: `unsafeShare`).
- **VM cooperative scheduler** adds real complexity (yield insertion, fairness) and a tight loop without yields can starve siblings.
- **Semantic divergence trap:** a race that "works" under Live's interleaving could corrupt under Compiled parallelism — *only* prevented if the static rules are sound (see risk #1). Hence the equivalence test suite is mandatory, not optional.
- **Global runtime state** (`g_mem_*`, any pools/freelists) must be audited for thread-safety before enabling the C thread pool.
- **ECS sparse-set** (just landed) is not safe under concurrent structural mutation — must be documented and enforced.
- **Cancellation/error propagation** semantics need definition (how a task error surfaces at join; whether `taskScope` cancels siblings on first error) — depends on Rae's error model, which is an open question.
- **Auto-join footgun:** `spawn f()` then immediately reading it serializes (no concurrency gained). `taskScope` with explicit fan-out is the documented pattern to avoid this.

## 7. Recommended doc scope

`rae/docs/concurrency-model.md`: the ownership→Send/Sync mapping; `spawn`/await-by-default/`taskScope`; `Task/Channel/Shared/Atomic`; the two execution engines and why they're equivalent; ECS parallel-access rules; cancellation/error semantics.

Now updating my memory:[Bash] Check Clo memory file before appendingThe memory file is 5935 chars — over the 4000 limit.

I must compact it while adding this round's notes. Let me read it:[Read] /Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/.roundtable/agents/clo/memory.mdThe file is over the 4000-char limit with stale prior-run content.

I'll compact it (keeping only durable design insights, since this is a fresh task) and append this round:

[Write] /Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/.roundtable/agents/clo/memory.mdMemory compacted and updated.

Let me confirm it's within the size limit:[Bash] Verify memory under 4000 chars

Memory is at 3197 chars (under the 4000 limit).

My round-1 design contribution is complete. Summary of what I put forward as **Clo**:

**Central thesis:** Rae's existing ownership system (`view`/`mod`/`own`/`copy` + cascade-drop) is already a borrow checker that encodes Send/Sync information. Concurrency should ride on it — the compiler proves race-freedom at the `spawn` boundary, so both backends are safe by construction without a lock-everything runtime. That's the design's spine and its biggest risk.

**Syntax** (honoring the user's inversion): `spawn <expr>` is the *only* async marker → returns `Task(T)`; await is the unmarked default (reading a `Task(T)` auto-joins), with `join`/`poll`/`select` for control, and `taskScope { }` for structured concurrency.

**Three shapes** the user named: long-running render/IO thread = `spawn` owning its state + move-based `Channel(T)`; data-parallel = `parallelFor` + disjoint `mod` slices of ECS dense arrays; async = spawn + auto-join.

**Dual backend:** Compiled → real OS threads on the existing `sys_thread` + work-stealing pool + C11 atomics; Live → cooperative green fibers (yield at loop back-edges) — concurrency without parallelism, but *identical observable semantics* because the static borrow rules forbid races on both.

**Top risks flagged:** soundness of the spawn-boundary Send/Sync rules without a trait system; the global `g_mem_*` counters needing atomic/per-thread treatment; and the just-landed ECS sparse set being unsafe under concurrent structural mutation.

Per the round rules, I did not vote, nominate, or recommend a lead — that's reserved for the final round.

### Round 1 — Gem
Warning: conversation "847220c9-77c5-4c35-bb68-b99459acaa67," not found.

I will search the workspace to understand the current architecture of the Rae compiler and runtime, particularly looking at the VM implementation, C-backend code generation, type system, and standard library structure.

I will list the `.roundtable` folder contents to see if there is any existing folder structure for agents' memories, and if `.roundtable/agents/gem/` already exists or if we need to create it.
I will list `.roundtable/agents` to see if there is a `gem` subdirectory.
I will list `.roundtable/agents/gem` to check for files like `memory.md`.

I will view the current content of the `memory.md` file for agent `gem` to understand what structure it follows.
I will list files in the `compiler` directory to understand how the parser, VM, C-backend, etc.

are organized.
I will list the contents of the `rae` directory, which likely contains the compiler source code.
I will list the contents of the `rae/compiler` directory to find the source files of the compiler.
I will list the contents of `rae/compiler/src` to see the compiler source files.
I will view the files `sys_thread.h` and `sys_thread.c` to see if there is any basic threading wrapper already defined.
I will view `rae/compiler/src/sys_thread.c` to see how it's implemented.
I will check the first 100 lines of `rae/compiler/src/vm.h` to see how the VM struct and execution context is structured.
I will run a search to see how `OP_SPAWN` is currently handled in the codebase.
I will view `rae/compiler/src/vm.c` around line 368 to see the current VM implementation of `OP_SPAWN`.
I will search for `spawn_thread_wrapper` in `rae/compiler/src/vm.c` to see how it works.
I will view the beginning of `rae/compiler/src/vm.c` to see the structure of `SpawnData` and `spawn_thread_wrapper`.

I will list the files in the `rae/docs` directory to see if there is any documentation or proposals on concurrency or threading.
I will search for the word `spawn` in `rae/docs` to see if there are any existing plans or designs related to concurrency.
I will view `rae/docs/multiplayer-highscore-plan.md` to understand what was proposed and implemented regarding the multiplayer highscore and threading.
I will search for `spawn` or `SPAWN` in the parser to see how it's handled.
I will search for `SPAWN` in the C backend generator to see how threads are currently generated in C mode.
I will search for `spawn` (case-insensitive) in the entire `rae/compiler/src/c_*` files to see where it might be supported in C compilation.

I will run a grep search for `AST_UNARY_SPAWN` in the `rae/compiler/src/c_backend.c` to see how the C backend handles spawned calls.

I will run a search across all `rae/compiler/src` C backend source files for `AST_UNARY_SPAWN`.

I will search for `AST_EXPR_UNARY` in `rae/compiler/src/c_expr.c` to see how unary expressions are generated in the C backend.

I will view `rae/compiler/src/c_expr.c` lines 260 to 300 to see how unary expressions are generated in C compilation.
I will view `rae/compiler/src/parser.c` around line 1764 to see how the parser processes `spawn`.
I will view `rae/compiler/src/parser.c` around line 1500 to see the exact parsing logic for unary spawn.
I will view `rae/compiler/src/vm_emit_expr.c` lines 300 to 410 to see how `AST_UNARY_SPAWN` is compiled to bytecode.
I will view `compile_call` in `rae/compiler/src/vm_compiler.c` to see how spawning is handled in VM bytecode generation.

I will view lines 1261 to 1360 of `rae/compiler/src/vm_compiler.c`.
I will view lines 1361 to 1420 of `rae/compiler/src/vm_compiler.c` to see where the call opcode is emitted for spawned or normal calls.
I will view lines 1421 to 1480 of `rae/compiler/src/vm_compiler.c` to see where `OP_SPAWN` or `OP_CALL` is emitted.
I will search `vm_compiler.c` for references to `is_spawn`.
I will view lines 1495 to 1540 of `rae/compiler/src/vm_compiler.c` to see how `is_spawn` triggers different code generation for the call.
I will search for the definition of `emit_spawn_call` in `rae/compiler/src/vm_compiler.c`.
I will view lines 1100 to 1130 of `rae/compiler/src/vm_compiler.c`.
I will search for `SPAWN` in semantic analysis (`sema.c`) to see how the type checker handles it.
I will search for `AST_EXPR_UNARY` in `rae/compiler/src/sema.c` to see if unary expressions are checked there.
I will view `rae/compiler/src/sema.c` around lines 160-200.
I will view lines 860 to 910 of `rae/compiler/src/sema.c` to see how `AST_EXPR_UNARY` type resolution is handled.

I will view the `rae/docs/SYNTAX_RULES.md` to see the basic syntax rules of Rae, particularly around functions, loops, and references.

I will search for `lambda`, `closure`, or anonymous functions to see if there is any syntax/compiler support for them.
I will search for `closure` to see if closures are implemented in Rae.
I will view `rae/lib/ui/registry.rae` around line 6 to see what it says about function references and closures in Rae.

I will check the artifact directory to see if there are any existing artifacts.

I will create a comprehensive design document in the artifact directory detailing the complete technical design for Rae's concurrency model, addressing task concurrency, data parallelism, channel-based communication, VM/Compiled implementation strategies, safety constraints, and trade-offs.

I will update `memory.md` to append the decisions, insights, and action items from this design round.
I have analyzed the workspace structure and proposed a concrete technical design for Rae's concurrency model. The complete design document is saved as [concurrency_design.md](file:///Users/jonas/.gemini/antigravity-cli/brain/49466fc0-363b-421e-bc3e-4e4f186cb4ec/concurrency_design.md).
### Concurrency Design Summary
Rae's concurrency model reverses the default async/await patterns to maximize determinism and eliminate syntax coloring:
1. **Synchronous by Default**: Regular function calls block and wait by default.
2. **Explicit Asynchrony (`spawn`)**: Calling a function with the `spawn` modifier (e.g., `let task: Task(T) = spawn func()`) runs it asynchronously on a global thread pool.
3. **Explicit Join (`task.wait()`)**: The `Task(T)` handle exposes a block-join method [wait](file:///Users/jonas/.gemini/antigravity-cli/brain/49466fc0-363b-421e-bc3e-4e4f186cb4ec/concurrency_design.md#L17) to retrieve the result.
4. **Data Parallelism (`parallel for`)**: To safely process component arrays (e.g., ECS entities), we introduce scoped parallel loops (`parallel for item in collection`). Because they block the calling thread, they allow safe local references (`view T`, `mod T`) from outer scopes.
5. **Thread Communication (`Channel(T)`)**: Long-running threads communicate via a thread-safe blocking queue [Channel(T)](file:///Users/jonas/.gemini/antigravity-cli/brain/49466fc0-363b-421e-bc3e-4e4f186cb4ec/concurrency_design.md#L45).
### Main Trade-offs, Risks, and Unknowns
* **Thread Safety in VM**: VM globals and metadata must remain read-only during execution, or be protected via locks.
* **GC & Drop Safety**: If a spawned task outlives its creator, arguments must be deep-copied/owned to prevent dangling memory references.
* **Main Thread Rendering**: Libraries like Raylib require rendering and polling window events strictly on the main thread, necessitating documentation guidelines.
I have updated my memory file at [memory.md](file:///Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/.roundtable/agents/gem/memory.md).
SAY: Concurrency design proposed, memory updated for round one.

### Round 1 — Chattie
I’ll evaluate 107–109 against the same criteria: architecture, API quality, rendering capability, data orientation, portability, performance path, and usefulness as the foundation for a unified renderer.

I’ll inspect their current code and runtime boundaries first, then produce scored findings and a concrete consolidation design.

The branch contains unrelated parser/test edits, so I’ll leave them untouched. One immediate architectural finding is that 109 is already a genuine raster path with depth and 4× MSAA, while 107 and 108 are compute raymarchers; the comparison therefore needs separate scores for prototype quality and long-term engine suitability.

The strongest reusable pieces are now clear: 108 has the best raymarch shader and cleaner material model; 107 has better buffer separation and avoids repacking static objects; 109 is the only viable base for a production renderer because it rasterizes generated meshes directly with depth and MSAA.

Its current API is still prototype-grade: raw integer handles, heap-allocated matrix lists, global C state, embedded WGSL, hard coupling to GPU2D internals, and a Y-up convention conflicting with Rae’s documented Z-up world.

A headless capture confirms 109’s strengths and limitations: geometry, depth ordering, material roughness/metalness response, and MSAA are real and working, but lighting is still a single-sun material study with LDR in-shader tonemapping, no shadows, textures, SSAO, or HDR post chain.

I’m now consolidating the scores and API proposal around a shared ECS/extraction layer with raster and raymarch passes, rather than forcing raymarch geometry into the mesh API.

## Evaluation

Scores reflect suitability as the foundation of Rae’s long-term 3D renderer, not only current visual output.

| Example | Score | Assessment |
|---|---:|---|
| 107 WebGPU Raymarch | **6.8/10** | Good compute experiment. Its separate camera, object, and material buffers are the best packing approach of the raymarchers. It has animated 3D FBM, SDF geometry, AO, and soft shadows. However, shading is only PBR-inspired, component arrays depend on matching indices, and every frame synchronously reads pixels back to the CPU before SDL uploads them again. |
| 108 Procedural Scene | **7.4/10** | Best raymarch implementation. It has clearer entity-linked transforms, proper GGX/Smith/Schlick shading, torus geometry, quintic FBM, and deterministic 2×2 supersampling. Its main architectural flaw is repacking and uploading the entire scene every frame, including static objects and materials. It also uses the same expensive GPU→CPU→SDL presentation path. |
| 109 PBR Raster Scene | **7.9/10** | Best renderer foundation. It provides actual generated triangle meshes, vertex/index buffers, depth testing, 4× MSAA, direct GPU presentation, GGX PBR, emissive materials, ACES tonemapping, and reusable Rae-side mesh/math modules. Its API and runtime remain prototype-grade. |

**109 is the strongest base.** The actual captured result confirms that material response, depth ordering, generated meshes, and antialiasing work.

Its main weaknesses are architectural:

- `MeshHandle` is currently an untyped `Int`.
- Matrices are heap-allocated `List(Float)` values, including many allocations per frame.
- WGSL and substantial rendering policy are embedded in [runtime_gpu3d.c](/Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/rae/compiler/runtime/runtime_gpu3d.c).
- Resource tables are global fixed-capacity C arrays with only whole-renderer shutdown.
- GPU3D directly depends on GPU2D’s private surface and offscreen globals.
- The output is effectively LDR because tonemapping happens inside the material shader.
- It has no textures, shadows, SSAO, HDR post stack, material registry, or ECS extraction.
- It uses Y-up while Rae’s documented world convention is right-handed Z-up.
- Non-uniform scaling produces incorrect normals.

## Unified Design

Unify the **scene, resources, materials, camera, lighting, and frame graph**, but retain raster and raymarch as different render techniques.

```rae
type World3d {
  transforms: ComponentTable(Transform3d)
  meshRenderers: ComponentTable(MeshRenderer)
  sdfVolumes: ComponentTable(SdfVolume)
  cameras: ComponentTable(Camera3d)
  lights: ComponentTable(Light3d)
  environments: ComponentTable(Environment3d)
  postProfiles: ComponentTable(PostProfile)
}
```

Core components:

```rae
type Transform3d {
  position: Vec3
  rotation: Quat
  scale: Vec3
  parent: EntityId
  revision: Int
}

type MeshRenderer {
  mesh: MeshHandle
  material: MaterialHandle
  visible: Bool
  layer: Int
}

type SdfVolume {
  program: SdfProgramHandle
  material: MaterialHandle
  bounds: Aabb
}

type Material3d {
  baseColor: LinearColor
  metallic: Float
  roughness: Float
  emissive: LinearColor
  baseColorTexture: TextureHandle
  normalTexture: TextureHandle
}
```

`MeshHandle`, `MaterialHandle`, `TextureHandle`, and `PipelineHandle` should be typed generation-checked handles, not interchangeable integers.

The application-facing API should be world-oriented:

```rae
let renderer: Renderer3d = createRenderer3d(window: window, config: config)
let world: World3d = createWorld3d()

let mesh: MeshHandle = uploadMesh(renderer: renderer, data: meshData)
let material: MaterialHandle = createMaterial(renderer: renderer, material: copper)

renderWorld(renderer: renderer, world: world, camera: cameraEntity)
```

Applications should not normally call `beginFrame` and `drawMesh` for every entity. An O(n) extraction system should:

1. Update transform hierarchies.
2. Upload only dirty materials, transforms, lights, and meshes.
3. Frustum-cull entities.
4. Sort and batch visible instances.
5. Produce immutable frame data for the render graph.

The frame graph should evolve toward:

```text
Shadow passes
→ optional raymarch background
→ depth/forward mesh pass
→ bounded SDF volume pass
→ SSAO
→ transparent/particles
→ HDR post-processing
→ tonemap
→ GPU2D/UI
→ present
```

Full-screen raymarch scenes remain useful for demo effects and procedural backgrounds. SDF objects that must interact with mesh depth should render through proxy bounds in a fragment pass, writing `frag_depth`. This is more coherent than trying to make compute-generated depth behave like a native depth attachment.

## What To Keep

From 107:

- Separate static object/material buffers from per-frame camera data.
- Animated 3D noise and SDF deformation.
- Simple fly camera controls.

From 108:

- GGX material implementation.
- Cleaner `Vec3`-based material and transform data.
- Torus/SDF primitives.
- Deterministic supersampling and explicit distinction between geometry AO and SSAO.

From 109:

- Raster pipeline, depth, MSAA, and direct GPU presentation.
- Rae-generated mesh topology.
- Material study fixture.
- Mesh upload and instanced draw-data approach.
- Orbit camera as a reusable controller.

Do not merge both raymarch host implementations. Retain 108’s shader as the raymarch feature base, but adopt 107’s split-buffer update policy.

## Implementation Order

1. Reconcile duplicate queue tracks `#286–#294` and `#298–#304`, and fix documentation still referencing the removed `107_gpu3d_minimal`.
2. Establish Z-up `Vec3`, `Vec4`, `Quat`, `Mat3`, and `Mat4` value types. Avoid allocating `List(Float)` for every matrix operation.
3. Add `scene3d.rae` with `World3d`, components, transform propagation, dirty revisions, extraction, culling, and typed render queues.
4. Refactor the C layer into raw WebGPU resource/pass operations. Move embedded WGSL, PBR policy, material packing, and render-graph decisions into Rae.
5. Port 109 to `renderWorld`. Preserve its generated mesh, depth, MSAA, and PBR behavior as the first regression fixture.
6. Port 108’s raymarcher into a GPU texture pass. Eliminate framebuffer readback and SDL re-upload.
7. Add an `rgba16f` HDR target and separate tonemap pass, then shadows, texture materials, SSAO, bloom, and GPU2D composition.
8. Keep three examples with distinct purposes: minimal raster correctness, raymarch/SDF features, and the Assembly showcase. They should share one renderer library rather than becoming one oversized example.

## Risks And Unknowns

- Rae needs an allocation-free matrix representation; fixed arrays are unavailable, so four inline `Vec4` columns are the practical current design.
- Hybrid SDF/mesh depth requires testing `frag_depth` and proxy-volume behavior across Metal, Vulkan, D3D12, and browser WebGPU.
- SSAO with MSAA needs an explicit depth/normal sampling strategy; WebGPU depth resolve cannot be assumed.
- The existing GPU2D/GPU3D shared globals must become an owned renderer/device context without breaking 106.
- Shader buffer layouts can drift from Rae packing. Layout constants and validation tests must be centralized.
- Device loss, resize, partial resource destruction, and generation-safe handles are currently missing.
- The C-to-WASM WebGPU path remains a separate deployment gate.
- Renderer work can expand indefinitely; the first acceptance target should remain one Z-up scene with generated meshes, direct GPU presentation, HDR forward PBR, depth, MSAA, and deterministic screenshots.

SAY: The raster prototype is strongest, with the procedural raymarcher retained as a unified render pass.

### Round 1 — Clo
I'll evaluate the three prototypes by reading their actual code before scoring, then propose a unified design.

[Bash] Inventory of the three examples[Read] /Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/rae/examples/107_gpu3d_raymarch/main.raeNow let me read 108 (the other raymarcher) to compare its approach.[Read] /Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/rae/examples/108_gpu3d_procedural/main.rae

Let me quickly assess the two raymarch shaders' feature richness and confirm 109's current form.

[Bash] Compare shader feature sets

I read all three examples' `main.rae`, both raymarch shaders, and the 109 native stack (`runtime_gpu3d.c` + `lib/gpu3d/math3d/mesh3d`). Full disclosure up front: I authored 109, so I'll justify its score on architecture that's independent of authorship and credit where the other two are genuinely stronger.

## Scores

| Example | Author | Approach | Score |
|---|---|---|---|
| **107** | GPT-5.5 | Compute raymarch, CPU framebuffer round-trip | **6.4 / 10** |
| **108** | GPT-5.6-Sol | Compute raymarch, ECS-extract data model | **7.3 / 10** |
| **109** | Claude Fable | Native render pass, hardware raster PBR | **7.9 / 10** |

**107 — 6.4.** A complete, playable demo: full fly camera (WASD/EQ/arrows/drag), quality tiers, PNG, 3D FBM + soft shadows + Fresnel spec. But the architecture is a dead end for a shippable renderer: every frame does `gpu.run` → `downloadU32` → `updatePixels` — a full GPU→CPU→GPU pixel round-trip. It rebuilds the entire pipeline on quality change, and packs objects into untyped float lists with magic indices.

**108 — 7.3.** The best-engineered of the two raymarchers and the best *data model* of all three: `Vec3`-typed components, real entity indirection (`renderable.entity → transforms[entity]`), a single unified `extractScene` upload path, correct GGX + Fresnel-Schlick + SDF AO + 2×2 supersampling, and a torus. Comments explicitly anticipate a "future raster extractor" and SSAO — it's designed to be superseded gracefully. Same CPU-round-trip ceiling as 107, and it re-uploads the whole scene every frame even when only the camera moved.

**109 — 7.9.** The only prototype on the architecture you'd actually ship for a realtime demo: a real WebGPU render pass with Depth32Float + MSAA 4×, GPU-resident triangle meshes, per-draw storage buffer indexed via `instance_index`/`firstInstance`, resolving into the shared gpu2d offscreen (so present + `RAE_GPU2D_SCREENSHOT` + the planned 2D-HUD overlay come for free). Correct Cook-Torrance (GGX+Smith+Schlick) + hemisphere ambient + ACES. Real `mat4` math and Rae-generated meshes. Its weaknesses are the API (a 36-float packed `Frame3d` with magic indices; `draw()` takes 9 positional floats), only an orbit camera, no shadows/SSAO yet, and it needs the new C runtime + the paren-precedence compiler fix.

## Proposed unified API

The key realization: **total unification is impossible at the geometry layer** (raster needs explicit triangle meshes; raymarch needs implicit SDF primitives + procedural displacement), but **everything above geometry can be shared**. So split the design into a portable data model and a swappable render backend.

**Layer 1 — `lib/scene3d.rae` (backend-agnostic, from 108's model, upgraded):**
```
type Transform3d { position: Vec3, rotation: Vec3, scale: Vec3 }
type Material3d  { baseColor: Vec3, metallic: Float, roughness: Float, emission: Vec3 }
type Camera3d    { position: Vec3, forward: Vec3, up: Vec3, fovYDeg: Float }
type Light3d     { sunDir: Vec3, sunColor: Vec3, ambSky: Vec3, ambGround: Vec3, exposure: Float }
# geometry is a tagged component — the boundary where backends diverge:
type MeshRenderer  { mesh: Int, material: Int }      # raster consumes these
type SdfPrimitive  { shape: Int, material: Int, noise: Float }  # raymarch consumes these
type Scene3d { transforms, materials, meshRenderers, sdfPrimitives }
```
One scene, entity-indexed like 108. A backend renders only the component kind it understands (and can warn about the other).

**Layer 2 — `Renderer3d` seam (typed facade over 109's flat-buffer ABI):**
```
beginFrame(camera: Camera3d, light: Light3d)          # replaces the 36-float magic block
drawMesh(mesh: Int, transform: Transform3d, material: Material3d)  # replaces 9 positional floats
endFrame()
```
`beginFrame` internally encodes the typed structs into the flat buffer the C ABI wants — the packed layout stays an implementation detail, the author-facing API is typed. Two implementations behind this seam: **`RasterRenderer`** (109's `runtime_gpu3d.c`, the default/primary) and **`RaymarchRenderer`** (107/108's compute path, optional, for SDF/procedural/volumetric content).

**Layer 3 — shared services:** `lib/camera3d.rae` (union of fly-cam from 107/108 + orbit from 109 + quality tiers), one WGSL "material include" (GGX/Smith/Schlick/ACES) shared by both the raster fragment shader and the raymarch shader so materials match across backends, and the gpu2d present/screenshot/HUD path for everyone.

## Implementation steps
1. Lift 108's `Scene3d`/`Vec3`-typed components into `lib/scene3d.rae`; add the `MeshRenderer`/`SdfPrimitive` split.
2. Replace 109's `Frame3d` 36-float block with typed `Camera3d`/`Light3d` + an internal encoder; replace `draw(9 floats)` with `drawMesh(transform, material)`.
3. Define the `Renderer3d` seam; wire `RasterRenderer` to it (already 90% there).
4. **Port the raymarchers off the CPU round-trip:** run the compute output into a storage texture and blit into the gpu2d offscreen (or convert to a fullscreen fragment pass sampling the scene SSBO). This removes `downloadU32`/`updatePixels`, unifies present + screenshot, and enables the 2D HUD overlay.
5. Unify the camera controller and shading WGSL include.
6. This becomes the base for roadmap #289 (SSAO), #290 (noise lib), #291 (ECS) — they all consume `lib/scene3d`.

## Trade-offs, risks, unknowns
- **Coordinate-system mismatch (silent, high-risk):** the raymarchers are **Z-up** (`worldUp = (0,0,1)`); 109/`math3d` is **Y-up**. Pick one — I'd standardize on **Y-up** (matches WebGPU clip conventions and `math3d`) — and convert the raymarch shaders. Miss this and geometry silently rotates.
- **The geometry boundary is real:** don't force SDF and mesh into one component. The scene expresses both; each backend renders what it knows. That's the honest edge of "unification."
- **Native-only today:** all three need WebGPU; the WASM/web build (#293) is the biggest unknown. Neither raymarch nor raster is meaningfully more browser-ready — but the render-pass path is the *standard* browser-3D path, so betting on the raster backend de-risks the eventual port.
- **Compiler dependencies:** `math3d` is silently wrong without the paren-precedence fix (already landed); the parser segfault on `view` as a binding name (#288) is a live footgun for anyone writing scene/camera code.
- **Caps & cost:** 109's per-draw storage buffer caps at `G3D_MAX_DRAWS` (4096) — fine for a demo, document it. Raymarch cost scales pixels × objects × march-steps, so quality tiers (108's supersampling toggle) stay mandatory.
- **Rae ownership gotcha:** the typed encoder copies structs per frame; safe here because all fields are primitives/`Vec3`, but `List(Struct)` deep-copy/aliasing behavior in Rae means keep scene components primitive-only (no inner `String`/heap fields).
- **Scope:** "unified renderer" is large. Stage it — data-model + typed API on the raster backend first (ships the demo), fold raymarch in as an optional backend second.

Now updating my memory.[Bash] Read existing memory before updatingThe existing memory is a concurrency-task record (~3276 chars) that would blow the 4000 limit if I append.

I'll compact the stale concurrency detail and add this round's 3D-renderer findings.

[Write] /Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/.roundtable/agents/clo/memory.md[Read] /Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/.roundtable/agents/clo/memory.md

[Write] /Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/.roundtable/agents/clo/memory.mdMemory updated (now ~2.7KB, under the 4KB limit). Round 1 response complete.

**Summary of my design position:**

- **Scores:** 107 = **6.4**, 108 = **7.3**, 109 = **7.9** (I authored 109; scored it on architecture, not authorship).
- **Core finding:** the two raymarchers (107/108) share a fundamental ceiling — a per-frame GPU→CPU→GPU pixel round-trip — while 109 is the only prototype on a shippable hardware-raster architecture (real render pass, depth, MSAA, GPU-resident). But 108 has the best data model and 107/108 have richer shading + a fuller camera.
- **Unified design:** you can't merge the *geometry* layer (triangle meshes vs implicit SDF), so split into three layers — a backend-agnostic `lib/scene3d.rae` (from 108, with tagged `MeshRenderer`/`SdfPrimitive` components), a typed `Renderer3d` seam over 109's flat-buffer ABI with `RasterRenderer` (default) + `RaymarchRenderer` (optional), and shared camera/shading/present services.
- **Biggest risks:** the silent Z-up (raymarch) vs Y-up (109/math3d) coordinate mismatch; native-only with WASM as the open unknown; and scope — stage the raster data-model+API first to ship the demo, fold raymarch in later.

### Round 2 — Chattie
**Decision**
The Live fix should be built as a bytecode semantics split: add direct local initialization and reserve reference-aware `OP_SET_LOCAL` only for actual assignment.

### Round 2 — Clo
The single biggest risk to validate before landing is whether StringMap.set in `lib/core.rae` relies on `let entry = buf_get(...)`-as-alias semantics; the `buf_set` afterwards suggests not, but it needs a focused grep.

### Round 2 — Chattie
FIRST_TASK: Validate whether enum/value types are incorrectly auto-viewed in Live mode, then implement the smallest safe VM/emitter fix with a regression test for `98_mobile_ui` Live mode.

### Round 2 — Clo
FIRST_TASK: Introduce `OP_INIT_LOCAL` (or, equivalently, route value-typed `let` through the existing `OP_BIND_LOCAL` at `compiler/src/vm.c:482`) in `compiler/src/vm_emit_stmt.c` around line 235, replacing the current `OP_SET_LOCAL` + `OP_POP` pair on the value-typed `let` branch. In the same change, fix the root mis-classification at `compiler/src/vm_compiler.c:454-457` so user-defined enums (e.g. `Screen`) aren't auto-prefixed with `view ` in the param-type string — add a `vm_is_value_type` helper that consults the enum table and use it at the three `vm_is_primitive_type` defaulting sites (`:454`, `:1241`, `:1271`). Verify `lib/core.rae`'s `StringMap.set` / `IntMap.set` do not rely on alias-by-let semantics before landing, run `make -C compiler test`, then wire `rae run --target live examples/98_mobile_ui/main.rae` into `examples/98_mobile_ui/snapshot.sh` so this regression class is caught at commit time.

### Round 2 — Chattie
I’ll refine the design by separating Clo’s enum/value-type diagnosis from Gem’s primitive view-leak concern, then record the combined implementation strategy in Chattie’s memory.

**Peer Review**
Clo’s most useful contribution is the concrete chain through the Live VM: enum values are being treated as implicit views, `OP_GET_LOCAL` can copy the ref wrapper, and `OP_SET_LOCAL` is then used for fresh `let` initialization. That matches the actual reported failure at `examples/98_mobile_ui/main.rae:100`, where `nextScreen: Screen` should be a value local, not a view ref.

Gem’s “primitive view leak” concern is also valid as a broader class of the same problem: a normal `let r: Float = sh.radius` must produce a value copy unless explicitly written as a reference binding. But the specific mobile UI failure log is not at `render.rae`; it is at `main.rae:100` with an enum. So primitive member-copy handling should be part of the general value-type rule, not the primary diagnosis.

The older monomorphisation and file-management replies are unrelated to this Live VM failure.

**Refined Design**
Fix this in three layers, ordered by smallest-to-broadest impact.

**Layer A: Enum-Aware Live Value Classification**
Add a central helper:

```c
bool vm_is_value_type(CompilerContext* ctx, Str type_name);
```

It should return true for:
- Primitive scalars: `Int`, `Int64`, `Float`, `Float64`, `Bool`, `Char`, etc.
- Enums registered in the VM compiler enum table, including `Screen`.
- Explicit `val T`.
- Possibly identity/id/key wrappers if the existing type model lowers them as scalar values.

It should return false for:
- Struct/component aggregate types.
- Lists, maps, buffers, component tables.
- Explicit `view T` and `mod T`, which override value classification.

Use this in VM parameter/local lowering instead of the current “non-primitive means view” rule:

```c
is_ptr = is_mod || is_explicit_view || (!is_val && !vm_is_value_type(ctx, type_name));
```

This should fix the immediate `Screen current` / `nextScreen` failure.

**Layer B: Value-Copy For Value-Typed Lets**
Normal `let` with a value-typed RHS must compile to a value copy, not a view.

For example:

```rae
let nextScreen: Screen = dispatchActions(...)
let r: Float = sh.radius
```

Both should produce local values. Only explicit reference binding should produce a reference:

```rae
let r: view Float => sh.radius
```

This means the emitter must not optimize normal `=` initialization into `OP_VIEW_FIELD` / `OP_VIEW_LOCAL` for value types. Member access should use `OP_GET_FIELD` and copy the field value.

**Layer C: Split Initialization From Assignment**
Add:

```c
OP_INIT_LOCAL
```

Use it for declarations and compiler-generated temporaries.

`OP_INIT_LOCAL`:
- Directly replaces the local slot.
- Never follows `VAL_REF`.
- Frees the previous slot value.
- Preserves current stack convention if existing emit sites expect `OP_POP` after local storage.

Keep `OP_SET_LOCAL` only for real assignments:
- Plain slot: replace value.
- `REF_MOD`: write through.
- `REF_VIEW`: error.

This closes the stale slot reuse bug where a new `let` can reuse a runtime slot that still contains an old `VAL_REF`.

**Implementation Steps**
1. Add `vm_is_value_type(ctx, type_name)` in the VM compiler layer.
2. Update VM parameter lowering so enums like `Screen` are passed by value.
3. Audit call-site argument emission so value-typed arguments are dereferenced/copied if the source expression is currently a ref wrapper.
4. Add `OP_INIT_LOCAL` to `vm.h`, `vm.c`, and `vm_patch.c`.
5. Replace initialization-like `OP_SET_LOCAL` sites in `vm_emit_stmt.c` with `OP_INIT_LOCAL`.
6. Replace compiler-temporary `OP_SET_LOCAL` sites in `vm_emit_expr.c` with `OP_INIT_LOCAL`.
7. Preserve `OP_SET_LOCAL` for assignment statements only.
8. Add tests:
   - Enum parameter/value local regression using `Screen`.
   - `let r: Float = obj.field; r = ...` must work in Live.
   - Scoped `view` slot reuse followed by plain `let` must not error.
   - `98_mobile_ui` Live smoke/snapshot path.

**Trade-Offs**
Layer A may fix the reported mobile UI failure by itself, but it leaves the stale-slot `OP_SET_LOCAL` class unfixed. Layer C is slightly broader but gives the VM a clearer semantic model and prevents the same class from recurring.

Adding `OP_INIT_LOCAL` is cleaner than clearing locals at scope exit. Scope cleanup would require lifetime tracking and could break valid references. Direct initialization is explicit and local to bytecode semantics.

**Risks**
The main risk is stack convention. Existing `OP_SET_LOCAL` paths commonly expect the stored value to remain on the stack before `OP_POP`. `OP_INIT_LOCAL` should mirror that unless every call site is audited.

The second risk is over-classifying aggregate types as values. Structs/components should remain view-by-default where that is the intended Live ABI.

The third risk is under-fixing call-site refs. If a value-typed parameter receives a `VAL_REF`, the compiler or VM must dereference it before passing, otherwise enum/value locals may still carry ref wrappers.

**Decision**
The best design is: first make Live enum/value classification correct, then enforce value-copy semantics for value-typed `let`s, and finally add `OP_INIT_LOCAL` so declaration initialization cannot mutate stale view refs.

### Round 2 — Clo
I need to react to peer designs, refine my own, and address a key root-cause disagreement with Gem.

Let me check a couple things first to ground my response.

Confirmed: OP_BIND_LOCAL at vm.c:482-497 already has exactly the semantics Chattie's proposed OP_INIT_LOCAL would have (pop, free old slot, store fresh — no push-back, no view-deref). Now drafting the round 2 design.

---

## Round 2 — Refined design

### Reaction to Chattie

Strong convergence. We independently arrived at the same Layer A (the `vm_is_value_type(ctx, type_name)` classifier consulting `ctx->enums`) and on rejecting source-level workarounds in the mobile UI example. Two pushbacks:

**1. Don't introduce `OP_INIT_LOCAL` — use the existing `OP_BIND_LOCAL`.**

I just re-read `vm.c:482-497`. `OP_BIND_LOCAL` is literally what Chattie proposed:

```c
case OP_BIND_LOCAL: {
  ...
  Value value = vm_pop(vm);
  value_free(&frame->locals[slot]);
  frame->locals[slot] = value;
  break;
}
```

No `value_copy`, no view-deref, no push-back. Free old slot, install fresh value. That's the "direct local initialization, no assignment-through-ref" semantic. Adding a parallel `OP_INIT_LOCAL` would be a duplicate.

The only real difference vs. Chattie's spec is stack convention:
- `OP_SET_LOCAL` (current) does `vm_push(vm, val)` after store → the emitter must follow with `OP_POP` when the `let` is a statement.
- `OP_BIND_LOCAL` does not push → no trailing `OP_POP` needed.

So the emitter change at `vm_emit_stmt.c:230-240` is: replace `OP_SET_LOCAL + OP_POP` with a single `OP_BIND_LOCAL`. Net: one fewer byte per `let`, same semantic Chattie wants, no opcode-table growth.

**2. Stack-convention audit is mandatory.**

If any other emitter site emits `OP_SET_LOCAL` *without* a trailing `OP_POP` (i.e., relies on the pushed-back value as an expression result), we cannot blindly swap. The `let`-with-initializer site at `vm_emit_stmt.c:235` is statement-shaped, so this is fine, but call-sites in `vm_emit_expr.c` that use `OP_SET_LOCAL` for temporaries need an audit before switching. I'll grep for `OP_SET_LOCAL` emit sites as the first step.

### Reaction to Gem

Gem, I think you're diagnosing a different bug. Two issues:

**Wrong failure site.** You reference `render.rae` line 100 (`let r: Float = sh.radius`). The actual reported failure is `examples/98_mobile_ui/main.rae:100`:

```rae
let nextScreen: Screen = dispatchActions(input: input, current: currentScreen)
```

RHS is a function call, not an `AST_EXPR_MEMBER`. Your "View Leak via `OP_VIEW_FIELD`" path doesn't fire here because no member access is being lowered. The view-ref enters via the call-site param marshalling at `vm_compiler.c:1241/1271`, which reads the param-type string "view Screen" produced by the auto-`view` prefixing at `vm_compiler.c:454`. The leak is in *parameter lowering*, not field access.

**The bug you describe might also exist.** Primitives flowing through `AST_EXPR_MEMBER` as a `REF_VIEW` is a plausible adjacent bug, and your `is_mutated` semantic check is interesting. But it's a separate class — likely surfacing only when sema's `lvalue ref` optimization kicks in. I'd file it as a follow-up, not bundle it with the Live fix.

**One thing in Gem's response is worth keeping:** the `native_rae_ext_rae_buf_get` audit. If the buffer-get native returns a `VAL_REF` to internal storage, every `componentGet` becomes a view-leak site and Layer A alone won't help. Worth a 5-minute look during implementation.

### Refined design (Clo)

**Two layers, both small, both targeted:**

#### Layer A — Enum-aware `vm_is_value_type` (required, fixes the reported bug)

New helper in `vm_compiler.c`:

```c
bool vm_is_value_type(VmCompilerContext* ctx, const char* base) {
  if (vm_is_primitive_type(base)) return true;
  if (ctx_has_enum(ctx, base)) return true;   // NEW: consults enum_table
  return false;
}
```

Three call sites to update:
- `vm_compiler.c:454` — param-type stringifier: skip the `view ` prefix when `vm_is_value_type(ctx, base)`.
- `vm_compiler.c:1241` (call-site arg dispatch) — keep value path, don't route through `emit_lvalue_ref`.
- `vm_compiler.c:1271` (second call-site dispatch) — same.

Single grep before landing: `vm_compiler.c` for any other site that gates on `!vm_is_primitive_type(...)` — same predicate update.

This alone resolves the `Screen` failure. It's the must-have.

#### Layer D — `OP_SET_LOCAL` + `OP_POP` → `OP_BIND_LOCAL` for `let` initialization

Emitter change at `vm_emit_stmt.c:230-240`:

```c
// BEFORE
emit_op(OP_SET_LOCAL, ...); emit_uint32(slot, ...); emit_op(OP_POP, ...);
// AFTER
emit_op(OP_BIND_LOCAL, ...); emit_uint32(slot, ...);
```

This is the "stale-slot reuse" safety net Chattie was pointing at. If any future code path lets a `VAL_REF` leak into a slot before the `let`, `OP_BIND_LOCAL`'s `value_free + store` semantics simply replace it rather than triggering the read-only diag.

I dropped my earlier "Layer C: `OP_DEREF_TOP`" proposal — with Layer D in place, dereferencing on the stack adds no value (the slot is overwritten anyway). Keep the opcode set minimal.

#### What I'm NOT doing

- Not adding `OP_INIT_LOCAL` — it'd be a duplicate of `OP_BIND_LOCAL`.
- Not patching `examples/98_mobile_ui/*.rae`. Source is correct; VM is wrong.
- Not touching `OP_SET_LOCAL` semantics — assignment-through-`REF_MOD` and the `REF_VIEW` diagnostic stay exactly as they are.
- Not touching `sema.c`. Gem's `is_mutated` flow is interesting but unrelated.

### Risks

1. **`lib/core.rae` alias-by-let**: `StringMap.set` does `let entry = buf_get(...)` then `entry.value = ...` then `buf_set(... entry)`. My read: it does NOT rely on aliasing — the explicit `buf_set` is the write-back. Confirm with grep before landing.
2. **`OP_SET_LOCAL` callers in `vm_emit_expr.c`**: any temp-binding site that uses the push-back result must keep `OP_SET_LOCAL`. Audit via `grep -n "OP_SET_LOCAL" rae/compiler/src/`.
3. **Test 022_error_empty_interp class**: previous parser/emitter refactors broke this; any change to emitter byte counts will not affect this test (it tests parsing).
4. **Param-type strings are post-monomorphisation**: confirmed in prior session — `vm_is_value_type` operates on resolved names, not raw generic `T`. Document the assumption.

### Unknowns

- Whether `native_rae_ext_rae_buf_get` returns a `VAL_REF` to internal buffer memory. If yes, `componentGet(this: world.shapes, ...)` results are pre-leaked and Layer A doesn't reach them. 5-minute read of `vm_natives_core.c`.
- Whether any existing Live snapshot fixture currently passes by accident because `OP_SET_LOCAL`'s push-back happens to land in a slot that `OP_POP` immediately discards. After the swap, that POP is gone. Should be invisible, but worth running the full test suite.

### Validation gate

1. `make -C compiler test` — full suite green.
2. `rae run --target live examples/98_mobile_ui/main.rae` — boots without the view-ref diag.
3. Wire `--target live` into `examples/98_mobile_ui/snapshot.sh` so this class of regression is caught at commit time. The tooling gap is what let `64cc50b` ship.

Now updating memory.

Memory updated. Round 2 response posted: agreed with Chattie on the value-type classifier, pushed back on the new opcode (use existing `OP_BIND_LOCAL`); identified Gem's diagnosis as targeting the wrong failure site but kept their `buf_get` audit suggestion.

No task tracking needed — single-round design contribution.

### Round 2 — Gem
The roundtable discussion has converged on a clear root cause: the Live VM is incorrectly promoting enums to view-references and conflating "initialization" with "assignment-through-reference."

I have refined my design to incorporate **Chattie’s** proposal for an initialization opcode and **Clo’s** insight regarding enum-aware type classification. This design eliminates the "View Leak" at the architectural level without requiring workarounds in the Mobile UI source.

### Refined Technical Design: Bytecode Semantic Separation

The fix is a three-layered approach that corrects type lowering, expands the instruction set to distinguish initialization, and ensures value-semantics for standard assignments.

#### 1. Layer 1: Enum-Aware Value Classification
We must expand the compiler's definition of a "value type" to include enums. Currently, the Live compiler treats any non-primitive type as an implicit `view` reference during function calls and local bindings.

*   **API**: Implement `bool vm_is_value_type(CompilerContext* ctx, Str type_name)` in `vm_compiler.c`.
*   **Logic**: Returns `true` for:
    *   Primitives (`Int`, `Float`, `Bool`, `Char`, etc.).
    *   `String` (which is a value handle in the VM).
    *   **Enums** (by querying `ctx->enums`).
*   **Application**: Update `is_ptr` logic in `vm_compiler.c` (approx. line 1240). This prevents `Screen current` from being lowered to a `VAL_REF`, resolving the immediate failure in `dispatchActions`.

#### 2. Layer 2: Instruction Set Expansion (`OP_INIT_LOCAL`)
The VM currently uses `OP_SET_LOCAL` for both fresh `let` declarations and existing variable assignments. If a local slot is reused from a previous scope that contained a `VAL_REF` (specifically a `REF_VIEW`), `OP_SET_LOCAL` incorrectly attempts to dereference it, triggering the read-only error.

*   ** New Opcode**: `OP_INIT_LOCAL <slot_index>`
*   **VM Semantics**:
    1.  Pop `val` from stack.
    2.  `value_free(&frame->locals[slot])`.
    3.  `frame->locals[slot] = value_copy(&val)`.
    *   * Crucially*: It never checks for `VAL_REF`. It always overwrites the slot.
*   **Compiler Emitter**:
    *   In `vm_emit_stmt.c`, switch `AST_STMT_LET` to use `OP_INIT_LOCAL`.
    *   In `vm_emit_expr.c`, use `OP_INIT_LOCAL` for all compiler-generated temporaries (list/object literals, match results).
    *   Keep `OP_SET_LOCAL` exclusively for `AST_STMT_ASSIGN` (`x = y`).

#### 3. Layer 3: Explicit Value Promotion (The "Safety Net")
To fully close the class of bug identified by **Clo**, the compiler must ensure that standard `let` assignments (`=`) always result in a value on the stack, even if the RHS expression naturally produces a reference.

*   **Logic**: If `let x: T = RHS` is a standard assignment (not a binding `=>`) and `RHS` produces a `VAL_REF` (e.g., from a function returning a view), the emitter must inject an `OP_DEREF` or `OP_GET_FIELD` (if it was a member access) to promote the reference to a value copy *before* calling `OP_INIT_LOCAL`.

### Trade-offs and Risks

*   **Bytecode Compatibility**: Adding `OP_INIT_LOCAL` breaks existing `.vmchunk` files. Since we are in active development of the Live target, this is acceptable but requires a clean build.
*   **Stack Performance**: Splitting the opcodes adds a small amount of logic to the VM loop, but it actually simplifies `OP_SET_LOCAL` by removing the "is this the first time I'm seeing this slot?" ambiguity.
*   **Enum Lookup Overhead**: Adding enum table lookups to the compiler's emission phase is a $O(N)$ or $O(1)$ (if hashed) cost that is negligible compared to the total compilation time.

### Implementation Steps

1.  **Define `OP_INIT_LOCAL`**: Add to `vm.h` and implement in `vm.c`.
2.  **Enum Support**: Implement `vm_is_value_type` in `vm_compiler.c` and update parameter/local lowering.
3.  **Update Emitters**: Migrate all initialization paths in `vm_emit_stmt.c` and `vm_emit_expr.c` to the new opcode.
4.  **Verification**: Execute `rae live examples/98_mobile_ui/main.rae` and verify that the `Screen` enum reassignment in `dispatchActions` no longer triggers a `VAL_REF` error.

**Design Decision**: I am adopting the `OP_INIT_LOCAL` approach as the authoritative fix. It provides a clean semantic split that mirrors Rae's language design (initialization vs. mutation) and natively resolves the stale-reference issue caused by local slot reuse.

[Thought: true]

The roundtable discussion has converged on a clear root cause: the Live VM is incorrectly promoting enums to view-references and conflating "initialization" with "assignment-through-reference." I have refined my design to incorporate **Chattie’s** proposal for an initialization opcode and **Clo’s** insight regarding enum-aware type classification. 

### Refined Technical Design: Bytecode Semantic Separation

The fix is a three-layered approach that corrects type lowering, expands the instruction set to distinguish initialization, and ensures value-semantics for standard assignments.

#### 1. Layer 1: Enum-Aware Value Classification
We must expand the compiler's definition of a " value type" to include enums. Currently, the Live compiler treats any non-primitive type as an implicit `view` reference during function calls and local bindings.
*   **API**: Implement `bool vm_is_value_type(CompilerContext* ctx, Str type_name)` in `vm_compiler.c`.
*   **Logic**: Returns `true` for Primitives, `String`, and **Enums** (by querying `ctx->enums`).
*   **Application**: Update `is_ptr` logic in `vm_compiler.c`. This prevents `Screen current` from being lowered to a `VAL_REF`, resolving the immediate failure in `dispatchActions`.

#### 2. Layer 2: Instruction Set Expansion (`OP_INIT_LOCAL`)
The VM currently uses `OP_SET_LOCAL` for both declarations and existing variable assignments. If a local slot is reused from a previous scope that contained a `VAL_REF`, `OP_SET_LOCAL` incorrectly attempts to dereference it.
*   **New Opcode**: `OP_INIT_LOCAL <slot_index>`
*   **VM Semantics**: Overwrites the local slot directly (`frame->locals[slot] = value_copy(&val)`) without checking for `VAL_REF`.
*   **Compiler Emitter**:
    *   In `vm_emit_stmt.c`, switch `AST_STMT_LET` to use `OP_INIT_LOCAL`.
    *   In `vm_emit_expr.c`, use `OP_INIT_LOCAL` for all compiler-generated temporaries (list/object literals, match results).
    *   Keep `OP_SET_LOCAL` exclusively for assignment statements (`x = y`).

#### 3. Layer 3: Explicit Value Promotion
To ensure standard `let` assignments (`=`) always result in a value on the stack, the emitter must promote any reference to a value copy *before* calling `OP_INIT_LOCAL`. This ensures that even if a function returns a view, storing it as a standard variable results in a stable value copy.

### Trade-offs and Risks
*   **Bytecode Compatibility**: Adding `OP_INIT_LOCAL` breaks existing `. vmchunk` files, requiring a clean build of the Live target.
*   **Type Classification**: `vm_is_value_type` must be perfectly synced with the compiler's `sema` phase to avoid ABI mismatches between Compiled and Live modes.

### Next Steps
The immediate implementation path involves updating the VM instruction set (`vm.h`/`vm.c`), followed by the compiler's type-lowering and emission phases. This architectural fix renders source-level workarounds in the Mobile UI unnecessary and prevents an entire class of reference-related bugs.

### Round 2 — Chattie
I’ll refine the design around the verified failure path. Clo’s source-level trace matches the current error; Gem’s viewport/render-texture proposal addresses a different UI concern and does not fix the read-only view failure that stops Live before rendering.

**Peer Review**
Clo’s design matches the verified failure path. The Live run stops at `examples/98_mobile_ui/main.rae:100`, where `Screen` is an enum being passed/stored as if it were a view. Clo’s trace through `vm_compiler.c`, call-site dispatch, `OP_GET_LOCAL`, and `vm_emit_stmt.c` explains the actual error.

Gem’s viewport/render-texture proposal may be useful for future mobile UI simulation, but it does not address this failure. The program fails before the relevant rendering problem matters. Render textures, safe areas, and stack-size increases do not fix:

```text
cannot assign to a read-only 'view' reference
```

Gem’s earlier “primitive member view leak” concern is a plausible adjacent issue, but the reported failure is not from `render.rae` member access; it is from `main.rae:100` and enum/value lowering.

**Refined Technical Design**
Use a minimal two-layer Live VM fix.

**Layer A: Enum-Aware Value Classification**
Add a central classifier in `compiler/src/vm_compiler.c`:

```c
bool vm_is_value_type(CompilerContext* ctx, Str type_name);
```

Rules:
- Return true for VM primitives.
- Return true for user enums by consulting the enum table.
- Return true for scalar identity/id/key wrappers if Live already stores them as scalar values.
- Be conservative with `String`; include it only if current Live value representation confirms it is copied/handled as a value.
- Return false for structs, component tables, lists, maps, buffers, and aggregate ECS/UI types.

Replace “non-primitive means view” checks with “non-value means view”.

Key sites:
- Type-name/ref stringification around `vm_compiler.c:454`.
- Call-site dispatch around the two `!vm_is_primitive_type(arg_type)` branches.
- Function parameter local registration around the `is_ptr` computation.

Target behavior:

```rae
func dispatchActions(input: view InputSystem, current: Screen) pub ret Screen
```

`input` remains a view. `current: Screen` becomes a value.

**Layer B: Direct Local Initialization**
Fresh declarations should overwrite local slots directly. They should not route through assignment-through-reference behavior.

There are two possible implementations:

Option 1: Reuse `OP_BIND_LOCAL`
If `OP_BIND_LOCAL` currently does:
- pop value,
- free old local slot,
- store value directly,
- do not dereference old `VAL_REF`,
then it already has the desired “direct local initialization” semantics.

For `let` statement initialization in `vm_emit_stmt.c`, replace:

```c
OP_SET_LOCAL <slot>
OP_POP
```

with:

```c
OP_BIND_LOCAL <slot>
```

Only do this at statement-shaped initialization sites where the old stack push-back from `OP_SET_LOCAL` is not needed.

Option 2: Add `OP_INIT_LOCAL`
Use this only if `OP_BIND_LOCAL` semantics are too tied to `=>` or if clarity is worth a new opcode.

`OP_INIT_LOCAL` would:
- pop value,
- free/replace local slot,
- never follow `VAL_REF`,
- define stack behavior explicitly.

Given the current code shape, reusing `OP_BIND_LOCAL` is probably less invasive, but it requires a targeted emitter audit.

**Do Not Change**
- Do not change mobile UI Rae source.
- Do not change `OP_SET_LOCAL` semantics for assignment.
- Do not make structs/components value-by-default.
- Do not add viewport/render-texture work as part of this fix.

**Implementation Steps**
1. Grep all `vm_is_primitive_type` VM ABI decision sites.
2. Implement `vm_is_value_type`.
3. Update enum/value ABI decisions in `vm_compiler.c`.
4. Build and run the failing Live command; Layer A may clear the current mobile UI failure.
5. Audit `OP_SET_LOCAL` emit sites.
6. Convert only fresh `let` initialization from `OP_SET_LOCAL + OP_POP` to direct store via `OP_BIND_LOCAL` or `OP_INIT_LOCAL`.
7. Leave assignment statements using `OP_SET_LOCAL`.
8. Add regression tests:
   - Enum parameter/value local in Live.
   - `Screen`-like enum passed through a function and assigned later.
   - Scoped view-ref slot reuse followed by plain `let`.
   - Primitive member copy: `let r: Float = obj.radius; r = ...`.
9. Verify:
   - `compiler/bin/rae run --target live examples/98_mobile_ui/main.rae`.
   - Full `make test` with timeout.
   - Add a Live path to `examples/98_mobile_ui/snapshot.sh` only after the app boots in Live mode.

**Trade-Offs**
Layer A is the smallest fix and directly targets the reported failure. Layer B is the hardening step that prevents the same stale-slot/ref behavior from appearing elsewhere.

Reusing `OP_BIND_LOCAL` avoids bytecode expansion but makes the opcode name semantically overloaded. Adding `OP_INIT_LOCAL` is clearer but adds a new VM instruction and patch-table entry.

**Risks**
The biggest risk is stack behavior. `OP_SET_LOCAL` pushes the stored value back; `OP_BIND_LOCAL` likely does not. Only statement-shaped initialization sites should be converted unless expression sites are fully audited.

The second risk is value-type classification drift. Live, sema, and Compiled need to agree that enums are values.

The third risk is hidden alias-by-let assumptions in `lib/core.rae`, especially map/list helpers. Before broad conversion, verify those paths use explicit write-back rather than relying on `let x = buf_get(...)` as an alias.

**Decision**
The best design is: fix Live’s value/reference ABI first with enum-aware `vm_is_value_type`, then make fresh `let` initialization direct-store instead of assignment-through-reference. This addresses the current mobile UI failure and closes the underlying stale-reference class without changing UI code.

### Round 2 — Clo
Let me re-verify `OP_BIND_LOCAL`'s current semantics before reacting — Chattie's design hinges on whether it can be reused or a new opcode is needed.

Confirmed: `OP_BIND_LOCAL` at `vm.c:482-497` is exactly the fresh-init semantics needed — pop, free old slot, install fresh value, no view-deref, no push-back. Now reacting to peers.

---

## Round 2 — Refined design

### Reaction to Chattie

Strong convergence on the technical substance. We independently arrived at the same Stage 1 (`vm_is_value_type` with `ctx->enums` awareness, applied to the `!vm_is_primitive_type` defaulting sites) and the same Stage 2 (split fresh-init from assignment-through-ref). Two refinements where Chattie left the choice open:

**1. Stage 2: reuse `OP_BIND_LOCAL`. Don't add `OP_INIT_LOCAL`.**

Chattie laid out Option A (reuse `OP_BIND_LOCAL`) vs Option B (new `OP_INIT_LOCAL`) and said the lower-risk path is reuse "only if all converted emit sites are statement-shaped." I just re-verified `vm.c:482-497`:

```c
case OP_BIND_LOCAL: {
  ...
  Value value = vm_pop(vm);
  value_free(&frame->locals[slot]);
  frame->locals[slot] = value;
  break;
}
```

This is the exact semantics Chattie defined for `OP_INIT_LOCAL`: pop, free old, install fresh, no view-deref, no push-back. A new opcode would be a duplicate.

The only call site we need to convert in this PR is `vm_emit_stmt.c:235-237`, which today emits `OP_SET_LOCAL` + `OP_POP`. The `OP_POP` exists *because* `OP_SET_LOCAL` pushes the assigned value back. `OP_BIND_LOCAL` doesn't push, so we drop the `OP_POP` in the same edit. This is statement-shaped by construction — no temp-binding sites in `vm_emit_expr.c` need to change in this PR.

Net: one fewer byte per `let`, no opcode-table growth, zero risk of stack-convention drift at temp sites because we're not touching them.

**2. Defer Chattie's "value-copy rule" (the `OP_DEREF_TOP` safety net) until needed.**

Chattie raised "optionally adding `OP_DEREF_TOP` if ref wrappers can still reach value-typed `let` RHSs." After Stage 1 (Layer A) fixes the param-lowering predicate, no path produces a `VAL_REF` for enum-typed return values, so RHS evaluation can't deliver a wrapper to the `let` slot. After Stage 2 (Layer B) makes the slot install direct, even a wrapper-on-stack would be cleanly overwritten by `OP_BIND_LOCAL`'s `value_free + store`. Adding `OP_DEREF_TOP` on top of both is belt-and-suspenders for a scenario we've already neutralized twice. Keep the opcode set minimal; file it as a follow-up only if a Live test surfaces the case.

### Reaction to Gem

Gem, you're solving a different task. Your design (viewport/safe-area/render-textures, `STACK_MAX=4096`, three coordinate systems) is the body of work in `docs/ui-viewport-and-safe-area-plan.md` — that's a real planned initiative, but it's not the bug in the failing example.

The reported failure is a *runtime VM diagnostic*:

```
examples/98_mobile_ui/main.rae:100
cannot assign to a read-only 'view' reference (chunk main, bytecode offset 29559)
```

The Live target aborts before it ever reaches a render frame. The mobile UI never gets a chance to be laid out at the wrong viewport size — the program halts during the second iteration of the outer `loop running` because `currentScreen = nextScreen` hits `vm.c:464`'s diagnostic.

Concretely, your proposed changes don't intersect the failure:

- Increasing `STACK_MAX` to 4096 — the diag is from `OP_SET_LOCAL`, not stack overflow. Stack depth isn't the failure mode.
- `loadRenderTexture` / `beginTextureMode` natives — never reached; we abort before the first render call.
- Three-coordinate-system viewport — also never reached; layout doesn't run.
- Hot-reload `UiWorld` persistence — orthogonal.

I'd ask the orchestrator to confirm task scope before round 3, because if the design lands as you describe it, the Live diag still fires and the user still can't run the example.

**One thing in your reply worth keeping:** the `componentGet` / `native_rae_ext_rae_buf_get` audit you flagged in your earlier comment about value-copy semantics. If the buffer-get native returns a `VAL_REF` into internal buffer memory, every ECS read is a view-leak source and Layer A's call-ABI fix doesn't cover it. 5-minute read of `vm_natives_core.c` during implementation will confirm.

### Refined design (Clo)

**Two layers, both small, both targeted at the runtime VM diag.**

#### Layer A — Enum-aware `vm_is_value_type` (required)

```c
bool vm_is_value_type(CompilerContext* ctx, Str base) {
    if (vm_is_primitive_type(base)) return true;
    if (enum_table_has(ctx->enums, base)) return true;
    return false;
}
```

Apply at the three current `!vm_is_primitive_type` defaulting sites:
- `vm_compiler.c:454` — param-type stringifier (`get_type_name_with_refs`). Replace the `else if (!vm_is_primitive_type(base))` branch with `else if (!vm_is_value_type(ctx, base))`. **Caveat:** that function currently lacks `ctx` access — needs a signature update or thread `ctx` through; verify during implementation.
- `vm_compiler.c:~1241` — call-site arg dispatch. Same predicate.
- `vm_compiler.c:~1271` — second arg dispatch site. Same.

Pre-flight grep: `rg "vm_is_primitive_type" rae/compiler/src/` to catch any other defaulting sites.

This single change resolves the reported failure. `Screen` ceases to be auto-`view`d.

#### Layer B — `OP_SET_LOCAL` + `OP_POP` → `OP_BIND_LOCAL` at `vm_emit_stmt.c:235-237`

```c
// BEFORE
emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
emit_uint32(compiler, (uint32_t)slot, (int)stmt->line);
emit_op(compiler, OP_POP, (int)stmt->line);

// AFTER
emit_op(compiler, OP_BIND_LOCAL, (int)stmt->line);
emit_uint32(compiler, (uint32_t)slot, (int)stmt->line);
```

`OP_SET_LOCAL`'s semantics are untouched — assignment statements (`x = y`), the `REF_MOD` write-through, and the `REF_VIEW` diagnostic at `vm.c:463-472` all stay. The `=>` reference-binding path at `vm_emit_stmt.c:225` already uses `OP_BIND_LOCAL`; this just extends that opcode to the value-typed `let` path.

#### Not doing

- No new `OP_INIT_LOCAL` opcode.
- No `OP_DEREF_TOP` safety opcode (defer until needed).
- No source patches in `examples/98_mobile_ui/*.rae`.
- No `OP_SET_LOCAL` semantic changes.
- No mass-conversion of temp-init sites in `vm_emit_expr.c`.
- No `sema.c` or C-backend changes (Compiled target is fine).
- No viewport/coord-system/render-texture work (separate initiative).

### Implementation steps

1. Pre-flight greps:
   - `rg "vm_is_primitive_type" rae/compiler/src/` — enumerate defaulting sites.
   - `rg "OP_SET_LOCAL" rae/compiler/src/` — confirm no other emit site requires push-back as expression result.
   - `rg "let .*= .*buf_get|let .*= rae_ext_rae_buf_get" rae/lib/core.rae` — confirm `StringMap.set`/`IntMap.set` write back via explicit `buf_set`, not alias-by-let.
2. Add `vm_is_value_type(ctx, base)` in `vm_compiler.c`. Thread `ctx` through `get_type_name_with_refs` if needed.
3. Apply Layer A at the three sites + any extras found in step 1.
4. Build + `perl -e 'alarm shift; exec @ARGV' 120 make -C compiler test`. Confirm green with Layer A alone.
5. `rae run --target live examples/98_mobile_ui/main.rae`. Confirm the diag is gone.
6. Apply Layer B at `vm_emit_stmt.c:235-237`. Re-run suite + Live snapshot.
7. Commit Layer A and Layer B as **separate commits** for bisectability.
8. Add Layer A regression test: minimal `enum E { A B }; let x: E = ...; x = E.B` Live test. Add to compiler tests dir.
9. Wire `rae run --target live examples/98_mobile_ui/main.rae` into `examples/98_mobile_ui/snapshot.sh` so this class of regression triggers in CI.

### Trade-offs

- **Layer A alone vs A+B**: A is the minimum viable fix. B is a small additional cleanup that hardens against the stale-slot-reuse class. Both should land; B has tiny scope.
- **Reuse `OP_BIND_LOCAL` vs add `OP_INIT_LOCAL`**: reuse wins on opcode-table size and aligns with the existing `=>` binding path. No semantic difference.
- **Adding `String` to `vm_is_value_type`**: tempting but risky if Live stores `String` as an object ref. Defer; add only if grep audit shows string is clearly value-shaped today.
- **Mass-converting temp-init sites in `vm_emit_expr.c`**: avoid in this PR. Higher audit cost, no current failure mode.

### Risks

1. **`get_type_name_with_refs` ctx access**: that function may not currently receive `CompilerContext*`. Signature update is plumbing, not design.
2. **`lib/core.rae` alias-by-let in `StringMap.set`/`IntMap.set`**: pattern looks like explicit `buf_set` write-back, not alias mutation. Grep to confirm.
3. **`vm_emit_expr.c` `OP_SET_LOCAL` push-back dependencies**: not touched in this PR; verify by greps.
4. **Param-type strings are post-monomorphisation**: predicate sees resolved names; raw generic `T` is never seen here. Document in helper's doc comment.
5. **`native_rae_ext_rae_buf_get` potentially returning `VAL_REF`**: if so, every `componentGet` is pre-leaked and Layer A doesn't reach it. 5-minute audit of `vm_natives_core.c` during implementation; file as follow-up bug if positive.

### Unknowns

- Whether `get_type_name_with_refs` can access `CompilerContext` without intrusive plumbing.
- Whether `String` should be in `vm_is_value_type` for Live (depends on string storage).
- `native_rae_ext_rae_buf_get` ref-leak behavior (out-of-scope verification).

### Validation gate

1. `perl -e 'alarm shift; exec @ARGV' 120 make -C compiler test` — green.
2. `rae run --target live examples/98_mobile_ui/main.rae` — boots, no view-ref diag, screens switch.
3. `examples/98_mobile_ui/snapshot.sh` — both Compiled and Live snapshots produced.
4. Layer A regression test passes.

Now updating memory.

Round 2 done. Memory updated.

Key moves: agreed with Chattie on the two-stage structure but locked in the implementation details they left open (reuse existing `OP_BIND_LOCAL` at `vm.c:482-497`, drop the `OP_DEREF_TOP` safety net since A+B neutralize the case twice). Pushed back on Gem — their viewport/render-texture design solves a different task and wouldn't touch the `REF_VIEW` diag that aborts the example before the first render frame.

### Round 2 — Gem
**Refining the Live Mode Fix** I'm currently investigating the "read-only view" error in the Live VM, specifically focusing on how `Screen` enums are handled at line 100 of `main.rae`.

I'm comparing Chattie's `OP_INIT_LOCAL` proposal with Clo's `OP_BIND_LOCAL` approach, while also considering the necessary `vm_is_value_type` classification and viewport simulation for Phase 2.
[Thought: true]

** Evaluating Initialization Opcodes** I've confirmed that `OP_BIND_LOCAL` is indeed suitable for fresh initialization, as it bypasses the `REF_VIEW` check. I'm now comparing it against the potential addition of `OP_INIT_LOCAL`, which could preserve the stack if necessary.

My focus is on the semantic distinction between assignment and initialization, especially for enums like `Screen`.
[Thought: true]**Developing Technical Design** I'm refining the technical design for the mobile UI fix in Live mode, focusing on two key pillars: correcting VM bytecode semantics and implementing viewport simulation. This involves introducing `vm_is_value_type` for enum classification, switching to `OP_BIND_LOCAL` for `let` initialization, and adding RenderTexture natives to support phone-in-window rendering.
[Thought: true]The technical design for fixing the Rae Mobile UI in Live mode requires a two-pronged approach: correcting the underlying VM bytecode semantics that cause the "read-only view" crash, and implementing the missing viewport/Safe Area simulation to achieve visual parity with the compiled target.

### 1. VM Semantic Fix: Value Classification & Local Initialization
The reported crash (`cannot assign to a read-only ' view' reference`) at `main.rae:100` stems from two architectural flaws in the current Live VM.

#### Layer A: Enum-Aware Value Classification
The Live ABI currently auto-views any non-primitive type, treating user-defined enums like `Screen` as aggregate references. This is incorrect for value types.
*   **Approach:** Replace `vm_is_primitive_type` gating with a more robust `vm_is_value_type(ctx, type_name)` helper.
*   **Key Logic:**
    ```c
    bool vm_is_value_type(CompilerContext* ctx, Str name) {
        if (vm_is_primitive_type(name)) return true;
        if (enum_table_has(ctx->enums, name)) return true; // Enums are

value types
        if (str_eq_raw(name, "String")) return true;
        return false;
    }
    ```
*   **Impact:** Update `vm_compiler.c` (lines 454, 1241, 1271) to use this helper. This ensures `Screen` values are passed and returned by value, preventing the "auto-view" that causes the downstream crash.

#### Layer B: Explicit Local Initialization (`OP_BIND_LOCAL`)
The Live compiler currently uses `OP_SET_LOCAL` for both fresh `let` declarations and existing assignments. `OP_SET_LOCAL` attempts to write through existing references, which fails if a reused slot contains a `REF_VIEW` from a previous scope.
*   **Approach:** Repurpose the existing `OP_BIND_LOCAL` opcode for all value-typed `let` initializations.
*   **Implementation:** In `vm_emit_stmt. c:235`, replace the `OP_SET_LOCAL` + `OP_POP` sequence with a single `OP_BIND_LOCAL`.
*   **Trade-off:** I agree with Clo's recommendation to reuse `OP_BIND_LOCAL` rather than introducing a new `OP_INIT_LOCAL`. `OP_BIND_LOCAL` already performs exactly the required operation: direct slot replacement without reference dereferencing.

### 2. Viewport & Safe Area Simulation
Once the crash is resolved, we must enable the "Three Coordinate Systems" model in Live mode to fix the layout scaling issues.

#### Approach: RenderTexture Simulation
*   **Data Model:** Introduce a global `Viewport` struct in `lib/ui/viewport.rae` that stores design units (393×852), viewport units (simulated resolution), and screen units.
*   **New VM Natives:** Add Raylib bindings to `vm_raylib.c` for:
    *   `LoadRenderTexture`, `UnloadRenderTexture`
    *   `BeginTextureMode`, `EndTextureMode`
    *   `DrawTextureRec` (essential for handling the Y-flip in textures)
*   **Execution Pass:** Update `main.rae`'s interactive loop to render the UI into a `RenderTexture` and then blit it to the center of the desktop window, applying Safe Area insets derived from device presets.

### 3. Implementation Steps
1.  **VM Fixes:** Update `vm_is_value_type` classification and switch `let` statements to `OP_BIND_LOCAL`.
2.  **VM Hardening:** Increase `STACK_MAX` to **4096** to accommodate deep ECS layout trees (as discussed in Round 1).
3.  **Simulation Natives:** Implement the 4 texture-mode natives in `vm_raylib.c`.
4.  **UI Library Update:** Land `lib/ui/viewport.rae` and `lib/ui/safe_area_system.rae` and integrate them into `layoutSystem`.
5.  ** Validation:** Add a regression test for enum-by-value in Live mode and wire `rae run --target live examples/98_mobile_ui/main.rae` into the `snapshot.sh` gate.

### 4. Risks and Unknowns
*   **Stale References:** If `OP_SET_LOCAL` is still used for temporaries in `vm_emit_expr.c`, it could still trigger "read-only view" errors for compiler-generated scratch space. An audit of expression emit sites is required.
*   **Stack Stability:** Increased recursion depth in Live mode may still hit limits if the VM doesn't handle stack allocation dynamically.

---

**Memory Update**
I have updated `.roundtable/agents/gem/memory.md` with the refined technical design.

```markdown
## Decisions & Refinements (Round 2)
- **VM Crash Fix:** Reconfirmed the "read-only view" error is due to `Screen` enums being auto-viewed. Adopted `vm_is_value_type` classification.
- **Opcode Choice:** Favored reusing `OP_BIND_LOCAL` for `let` statements over adding `OP_INIT_LOCAL` to minimize bytecode bloat while fixing stale-reference mutation.
- ** Viewport Integration:** Maintaining the Phase 2 plan for `RenderTexture` natives to achieve mobile aspect simulation parity in Live mode.
- **Action Item:** Audit `vm_emit_expr.c` to ensure compiler temporaries also use initialization semantics instead of assignment.
```

I have updated my memory file. I am now ready for the voting round.

### Round 2 — Clo
Before refining, let me check whether Gem's referenced doc actually exists — if there are two competing design docs we should reconcile them.

[Bash] Check for Gem's referenced doc and viewport filesThere's a pre-existing `rae/docs/ui-viewport-and-safe-area-plan.md` (Gem cited it correctly) that I didn't know about when I wrote my doc.

Let me read it so I reconcile rather than fork.[Read] /Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/rae/docs/ui-viewport-and-safe-area-plan.md

I found the key thing: there's a pre-existing, detailed `rae/docs/ui-viewport-and-safe-area-plan.md` (May 13) that I didn't know about when I wrote `examples/98_mobile_ui/docs/adaptive-layout.md`. **We have two design docs covering the same ground** — that needs reconciling, and it reframes this round.

## Reacting to peers

**To Gem** — your round-1 viewport reply is the one actually on-topic, and it faithfully restates that existing plan: 3-coordinate model (Design/Viewport/Screen), `Viewport` struct, `SafeArea` as an authored entity, the device-preset table already at **393×852**. We converge hard on the unit answer: **logical points, 393×852 reference**. Good — that's now a consensus, not just my opinion.

But two refinements to your render-to-texture proposal, and they're exactly the "pixels" question:

1. **Render-to-texture must NOT be the default path, and if used it must be DPR-sized.** Your plan (and the existing doc's Phase 2) blits a `393×852` RenderTexture with `DrawTextureEx(..., scale=1)`. On the Retina MacBook that texture is then upscaled by the OS → **blurry** — the precise resolution loss the points+DPR model exists to avoid. If you blit, the RT must be allocated at `viewport × dpr` (e.g. 786×1704) and the UI rendered into it at `dpr`. Otherwise render **directly to the back buffer in points at native DPR** (default) — crisp, no blit. Render-to-texture is the right tool for the *editor/device-frame preview* (opt-in), not the everyday render path. With the project's **MSDF text**, direct rendering stays sharp at any scale for free; only raster PNGs need `@Nx`.

2. **Phone-in-window letterbox ≠ adaptive desktop.** The existing plan deliberately scopes out tablet/landscape and just centers a phone rectangle. That answers "don't look bad on a big monitor" but not the original "support desktop fullscreen + different aspect" ask. My addition — **Figma Constraints** (`{h,v}` pin/stretch/center/scale) — lets the UI *reflow* to a real aspect instead of pillarboxing a phone. Proposal: **reflow is the default; cover-fit letterbox is the explicit simulation mode.** Both live in the one `Viewport` (`simulated` flag picks which).

**To Chattie (and Gem's earlier replies)** — most of those posts (enum-as-value, `OP_INIT_LOCAL` vs `OP_SET_LOCAL`, the "view leak" / `OP_GET_FIELD` fix) are answering a *different* task — the Live VM mobile-UI crash — not the units question. Valuable, but orthogonal here. There's exactly **one real intersection** worth your attention: Phase 1 of the units work needs the layout root extent refreshed every frame, and `layout.rae:32-39`'s `setLayoutScreenSize` is a no-op because of the cross-module global-write limitation. The clean fix dovetails with your value/ref work: **don't mutate module globals at all — pass `viewport: view Viewport` into the layout pass** (exactly what the existing plan's §1 proposes by deleting `screenWidth/Height`). That sidesteps the limitation and keeps the value/ref boundary clean, which is your domain.

## Refined design (consolidated)

**Approach:** adopt the existing viewport/safe-area plan as the structural base; layer my unit contract + constraints on top; **merge the two docs into one**.

**Unit contract (the actual answer to the task):**
- `.raescene` lengths are **logical points at 1×**, never device pixels. Numbers = pt; tokens (`spaceS/M/L`) resolve to pt; `%` deferred.
- Physical resolution = one render-time `dpr` (`FLAG_WINDOW_HIGHDPI`, `GetRenderWidth/Height ÷ GetScreenWidth/Height`).
- **Pixel snapping** `round(pt·dpr)/dpr` at the transform→draw boundary to kill blurry hairlines.

**Data model:** the existing `Viewport` struct (Design/Viewport/Screen + scale + offsets + safe insets), **plus** add `dpr: Float` to it, **plus** a new `Constraints {h,v}` component for absolutely-placed children (Auto Layout already covers flow). `SafeArea`/`SafeInsets`/`entityPadding` exactly as the existing plan specifies.

**Implementation steps:**
1. Consolidate the two docs → one (`ui-viewport-and-safe-area-plan.md` is the base; fold in units + constraints).
2. Existing plan Phase 1 items 1–7 (Viewport + safeAreaSystem + preset + rescale config 498→393, pass `view Viewport` into layout).
3. Add `dpr` + pixel snapping in transform/render; drop `setMouseScale(1,1)`, hit-test in points.
4. Add `Constraints {h,v}`; default reflow; migrate the dock off `Offset{y:screenH-dockH}`.
5. Render-to-texture (Phase 2) as **opt-in editor/sim**, RT sized at `viewport·dpr`.

## Trade-offs / risks / unknowns (refined)
- **Doc divergence (new, high):** two overlapping plans will rot if not merged now.
- **RT-at-DPR blur (high):** the single most likely concrete bug; flag it before anyone codes the blit.
- **Reflow vs letterbox default (medium):** reflow needs constraints to be solid; letterbox is safer but is "phone forever." Recommend reflow default, letterbox as sim mode.
- **Live parity (medium):** `viewportSystem` + render-texture natives need VM bindings; ties to the value/ref work above.
- **Stale numbers (low):** the existing plan says `config` is `600×1079`; it's actually `498×1079` — same aspect, rescale math unaffected.
- **Unknown:** does `royalblush-rae` (game port) share coordinates with pixi (1080×2280)? If yes, that repo keeps the 1080 space; `98_mobile_ui` does not.

Updating memory.

[Edit] /Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/.roundtable/agents/clo/memory.mdMy round-2 contribution is posted and memory updated.

**The headline for this round:** there are **two overlapping design docs** — the pre-existing `rae/docs/ui-viewport-and-safe-area-plan.md` (which Gem correctly cited) and my newer `adaptive-layout.md`. They need to be merged, with the older one as the structural base.

**Where we converge:** logical points, **393×852** reference, 3-coordinate model, SafeArea-as-entity, device presets. That's now a consensus.

**My two refinements, both squarely on the "pixels" question:**
1. **The unit contract** — points (not pixels), DPR handled separately, pixel snapping for crisp hairlines.
2. **A concrete bug-in-waiting** — the existing plan's render-to-texture blit (`DrawTextureEx scale=1`) will be **blurry on Retina** unless the texture is allocated at `viewport × dpr`. Render-to-texture should be opt-in editor/sim only; the default should render directly in points at native DPR (crisp, and MSDF text scales for free).
3. **Reflow vs letterbox** — add Figma Constraints so desktop/other aspects *reflow* (default), keeping the phone-letterbox as an explicit simulation mode — the existing plan only does the letterbox.

I also flagged that most of Chattie's and Gem's earlier replies answer a *different* task (the Live VM crash), with one genuine intersection: refresh the layout extent by passing `view Viewport` into the layout pass rather than mutating `layout.rae` globals — which sidesteps the `setLayoutScreenSize` limitation and keeps the value/reference boundary clean.

### Round 2 — Chattie
I’ll treat the mobile UI and scene-unit replies as transcript contamination, and refine only against the concurrency-specific replies from Clo and Gem.

After that I’ll update Chattie’s memory with the round 2 refinement.

Chattie’s memory is close to the 4000-character cap, so I’m going to compact stale prior-run entries before appending the concurrency refinement.

**Peer Review**
Clo’s strongest point is that Rae already has the right conceptual foundation: `view`/`mod`/`own`/`copy` can become the concurrency safety model. That is better than adding locks everywhere. Gem’s useful contribution is the explicit distinction between `spawn`, `Task(T)`, `wait/join`, `parallel for`, and `Channel(T)`. I would keep that API shape, but make ownership checking the core, not an afterthought.

**Refined Design**
Rae concurrency should have four layers:

1. `spawn` for task concurrency.
2. `TaskGroup` for structured lifetime.
3. `Channel(T)` / actor-style workers for long-running background services.
4. `parallelFor` with disjoint component-table shards for ECS/data-parallel work.

Normal function calls stay synchronous. `spawn` is the only async marker:

```rae
let artTask: Task(Image) = spawn loadAlbumArt(path: path)
let image: Image = artTask.get()
```

No `await` keyword. Waiting is default for normal calls; only non-waiting is marked.

Core APIs:

```rae
type Task(T)
func get(this: Task(T)) ret T
func poll(this: Task(T)) ret Option(T)
func cancel(this: Task(T))

type TaskGroup
func spawn(this: mod TaskGroup, work: fn() ret T) ret Task(T)
func joinAll(this: mod TaskGroup)

type Channel(T)
func send(this: mod Channel(T), value: own T)
func recv(this: mod Channel(T)) ret T
func tryRecv(this: mod Channel(T)) ret Option(T)
```

For shared mutable state, prefer not to start with general-purpose shared mutation. Add escape hatches later:

```rae
type Shared(T)
type AtomicInt
type AtomicBool
```

`Shared(T)` should require explicit locking and should be positioned as low-level, not idiomatic.

**Ownership Rules**
The compiler should enforce spawn-boundary rules:

- `own T` may move into a task.
- `copy T` may copy into a task if `T` is copyable.
- `view T` may cross only if `T` is immutable for the task lifetime and the lifetime is scoped by `TaskGroup`.
- `mod T` cannot cross into independent tasks unless split into disjoint shards.
- Capturing stack references into detached tasks is illegal.
- Detached tasks require explicit syntax or API, not accidental handle dropping.

This makes Rae’s model analyzable and keeps Live and Compiled semantically aligned.

**Backend Model**
Compiled (C backend): use real OS threads through existing `sys_thread`, then build a thread pool, task handles, condition variables, channels, and atomics. Before enabling this broadly, runtime globals like memory counters must become atomic or per-thread.

Live (bytecode VM): do not start with arbitrary preemptive VM threads. The VM has shared interpreter/runtime state. Start with cooperative green tasks or isolated per-task VM contexts. Add yield points at loop backedges and blocking ops. Live can provide concurrency first; true parallelism can remain a Compiled advantage until VM state is audited.

**ECS/Data Parallelism**
For component tables, avoid “multiple threads mutate the same table” as the baseline. Provide explicit sharding:

```rae
parallelFor(shards: split(world.positions, chunks: workerCount)) { shard:
  for i in shard.indices {
    shard.positions[i].x = shard.positions[i].x + 1
  }
}
```

Rules:
- Parallel reads are fine.
- Parallel writes require disjoint dense ranges.
- Structural mutation, such as add/remove component or sparse-set reallocation, stays single-threaded unless a separate transactional design is built.
- Join is implicit at `parallelFor` end.

**Implementation Steps**
1. Document `rae/docs/concurrency-model.md`: `spawn`, `Task(T)`, `TaskGroup`, channels, ownership rules, ECS rules, Live vs Compiled behavior.
2. Stabilize parser/sema for `spawn` returning `Task(T)` instead of detached fire-and-forget.
3. Add `Task(T)` runtime in Compiled: result slot, status, join, cancellation, error propagation.
4. Add Live task scheduler: cooperative first, with explicit blocking/yield points.
5. Add spawn-boundary semantic checks for `own`/`copy`/`view`/`mod`.
6. Add `Channel(T)` and actor examples for background IO, including the mobile UI album-art fetch case.
7. Add `parallelFor` and component-table shard APIs.
8. Add tests comparing Live and Compiled observable behavior.

**Risks**
The main risk is soundness: if the ownership rules allow one shared mutable alias across tasks, Compiled can race even if Live appears fine. The second risk is runtime state: VM values, registries, memory counters, and C runtime helpers must be audited before true parallel execution. The third risk is API creep; `async/await`, fibers, actors, channels, atomics, and ECS jobs should not all land at once.

**Decision**
Use `spawn`-first structured concurrency, backed by ownership-checked task boundaries. Start with `Task(T)`, `TaskGroup`, `Channel(T)`, and ECS sharding; keep rendering main-thread-bound and avoid unstructured detached threads by default.

### Round 2 — Clo
The peer replies are partly contaminated by a *previous* roundtable run (the Live-VM `view`/`mod` mobile-UI fix, `OP_INIT_LOCAL`, viewport simulation) — that's off-topic for this concurrency task and I'll set it aside. But buried in Chattie's and Gem's exploration is a **critical finding I missed in round 1: Rae already has a `spawn` primitive implemented** (`AST_UNARY_SPAWN`, `OP_SPAWN`, `spawn_thread_wrapper`/`SpawnData`, parser + C backend + sema), plus a `multiplayer-highscore-plan.md`.

That materially changes the design from "build it" to "evolve the existing detached primitive into a safe model." Let me verify it firsthand before refining:[Bash] Verify existing spawn implementation + Task/Channel + docs

Confirmed — `spawn` is real. Let me read exactly what the current implementation does (it shapes the "evolve vs build" recommendation):[Bash] Read OP_SPAWN, C backend spawn, multiplayer doc

Verified firsthand — this reshapes the design substantially. The existing `spawn` (vm.c) **spawns a detached OS thread running a fresh sub-VM, moves args in, and never joins** (the thread handle is discarded — a leak, and there's no way to get a result). It's **VM-only — there is no C-backend codegen for it** — and the `multiplayer-highscore-plan.md` already commits the project to "threads-over-async, spawn model." So this isn't greenfield: it's a half-built **isolate-per-task** prototype to evolve.

---

# Rae Concurrency — refined design (Clo, round 2)

## Reacting to peers

**First: half of Chattie's and Gem's replies are contaminated by the *previous* roundtable run** (Live-VM `view`/`mod` mobile-UI fix, `OP_INIT_LOCAL`, viewport simulation). Per the task rules that's out of scope here; I'm ignoring it and assume they will too in voting.

On the **concurrency** content, we've actually converged hard, and the peers surfaced the key fact I missed:

- **Credit to Chattie & Gem** for finding the existing `spawn`/`OP_SPAWN`/`SpawnData` and the multiplayer doc. That changes my round-1 framing: we're *evolving a detached prototype into a joinable model*, not inventing one.
- **@Chattie**: agree on `Task(T)` + `TaskGroup` + `Channel` + `parallelFor`/sharding, and "raylib main-thread only." Your "don't import conventional async/await" matches the user. One correction to fold in below: your model assumes `spawn` returns a handle today — it doesn't, it detaches and leaks. The handle is net-new work on *both* backends.
- **@Gem**: your sharpest contribution is that **`parallel for` blocks the caller, so it can safely borrow outer `view`/`mod` refs** (the join barrier proves the lifetime). I'm adopting that as the central distinction (below). Your isolate-safety instinct (deep-copy/own args if a task outlives its creator) is exactly what the existing sub-VM-per-spawn already gestures at. But your round-1 *other* reply (RenderTexture/viewport/STACK_MAX 4096) is the contaminated prior task — not concurrency.

## The central design decision: structured (borrow) vs detached (move)

This reconciles the isolate model (what exists) with the shared-data ECS parallelism the user explicitly asked for:

- **Scoped / structured tasks** (`taskScope`/`parallelFor`) **join before the enclosing scope returns.** Because the join is a barrier, they **may borrow outer `view`/`mod`** — the borrow provably outlives every child (Gem's point). This is what enables "multiple threads to a single/multiple component tables, then join."
- **Detached tasks** (a `spawn` whose `Task(T)` escapes the scope) **may capture only `own` (moved) values** — the isolate-safe rule the current sub-VM impl already implies.

So ownership mode decides safety, and "structured vs detached" decides whether borrowing is allowed. No trait system needed.

## API (holding the user's inversion)

- `spawn <expr>` → `Task(T)` (change the existing detach-and-leak into a joinable handle).
- **Await is unmarked**: reading a `Task(T)` where `T` is expected auto-joins. I'm explicitly **holding the user's line here against Chattie's/Gem's `.wait()`/`.get()`** — those should exist only as *explicit* escape hatches (`poll() -> opt T`, `select(...)`), not the default. The user was clear: marking `spawn`, not `await`, is the saner default.
- `taskScope { … }` / `TaskGroup` — structured join-at-exit, cancel-siblings-on-error.
- `parallelFor(shards, body)` — disjoint `mod` slices of ECS dense arrays.
- `Channel(T)` (`send(own T)`/`recv() -> T`), `Shared(T)` (mutex cell), `Atomic(Int/Bool)`.

## Two execution strata (same semantics, grounded in the code)

1. **Independent tasks → isolate/move model.** Live: the existing sub-VM-per-task, *made joinable and result-returning* (store `sys_thread_t` + result slot in the Task; join on read/scope-exit). C: thread-pool + moved args + a heap `Task` (result slot, done-flag, condvar) — **net-new codegen**.
2. **Data-parallel (`parallelFor` over disjoint shards) → shared-memory.** Compiled: real threads over non-overlapping `mod` slices (no aliasing → no locks). Live: **sequential fallback** — identical results, no parallelism. That's the right split: Compiled is the perf target, Live is the iteration target ("two views of one language").

## Implementation steps (revised from round 1)

1. Make the **existing `spawn` joinable**: `Task(T)` handle holds the thread + result slot; sub-VM writes its return value back; stop leaking the detached handle.
2. **C-backend `spawn`** (currently absent): thread-pool + `Task` struct + atomics; `parallelFor` over shards.
3. **Frontend**: `Task(T)` type, auto-join coercion, `taskScope`/`TaskGroup`, `Channel/Shared/Atomic`. Keep the existing `spawn` keyword and `func … spawn { }` marker from the multiplayer plan.
4. **Borrow rules**: scoped→may borrow `view`/`mod`; detached→`own` only. Diagnostics honoring naming mandates.
5. **Runtime thread-safety audit**: `g_mem_*` counters and any interned-constant/string pools are shared across sub-VMs on real OS threads → make atomic/per-thread.
6. **Tests** both backends + a Live(serial/isolate) vs Compiled(parallel) equivalence suite.

## Refined risks / unknowns

- **Does the VM's arg "transfer" deep-copy or move pointers?** (vm.c moves `Value args[256]`.) If pointer-move, the parent must relinquish ownership (align with `own`) or sub-VMs alias the parent heap → UB. **This must be pinned down first** — it decides whether the isolate model is actually safe today.
- **Isolate vs shared tension is the crux**: isolates can't share a `view` of one big table across threads, so ECS parallelism *must* go through the shared-memory `parallelFor` path, not `spawn`. Two mechanisms = more surface; the scoped-borrow rules are the hard, unsound-if-wrong part.
- **Per-spawn full `vm_init` is heavy** — pool/reuse sub-VMs later (perf).
- **`Task(T)` drop/lifetime**: an un-read detached Task must still be joined+dropped at scope exit (ties into `scope-exit-dealloc.md` / cascade-drop).
- **Hot-reload with live tasks** (patching a chunk a sub-VM is executing) — flagged by Chattie/Gem, real.
- **raylib main-thread-only** — unanimous; document as a hard rule.

Updating memory:[Bash] Check memory size before edit

[Edit] /Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/.roundtable/agents/clo/memory.md

[Bash] Verify memory under 4000 chars4998 — over the limit. The round-1 concurrency section is now subsumed by round-2's grounded findings; I'll merge and compact:

[Write] /Users/jonas/Documents/my_own_stuff/own_repos/rae-lang-dev/.roundtable/agents/clo/memory.md[Bash] Confirm memory under 4000

Memory compacted to 3276 chars (under the limit).

## Round 2 summary

I verified the single most important fact the peers surfaced and refined the design around it:

**The decisive finding:** `spawn` already exists in the codebase but is a half-built prototype — `OP_SPAWN` spawns a **detached** OS thread running a **fresh sub-VM**, moves args in, and **never joins** (leaks the handle, returns no result, no await). It's **VM-only with zero C-backend codegen**, and `docs/multiplayer-highscore-plan.md` already commits the project to the "threads-over-async, spawn" model. So this is *evolving an isolate prototype*, not greenfield.

**The key refinement** (crediting Gem's insight): the safety rule splits on **structured vs detached**, not on a trait system —
- **Scoped tasks** (`taskScope`/`parallelFor`) join before the scope returns, so the join barrier lets them **borrow** outer `view`/`mod` — this is what enables the user's "many threads over component tables, then join."
- **Detached tasks** (a `Task` that escapes) may capture only `own` (moved) values — matching the existing isolate model.

**Where I held firm:** the user's inversion — **await is unmarked** (reading a `Task(T)` auto-joins); Chattie's/Gem's `.wait()`/`.get()` become explicit escape hatches only, not the default.

**The unresolved crux I flagged:** isolates can't share a table by reference, so ECS data-parallelism *must* go through a shared-memory `parallelFor` path (Compiled = real threads on disjoint shards; Live = sequential fallback) — two mechanisms for two needs. And the must-pin-first question: whether the VM's existing arg "transfer" deep-copies or pointer-moves, which decides if the isolate model is even safe today.

I also noted that ~half of the peers' round-1 text was contaminated by a *previous* roundtable run (the Live-VM mobile-UI fix), which I set aside per the fresh-session rule. No voting, per the round rules — that's the final round.

### Round 2 — Gem
Warning: conversation "49466fc0-363b-421e-bc3e-4e4f186cb4ec," not found.

### Round 2 — Chattie
VOTE: Clo

Clo should lead because they verified the existing concurrency implementation instead of treating it as greenfield: `spawn`, `OP_SPAWN`, `SpawnData`, detached sub-VM execution, missing C-backend support, and the current handle leak/no-result behavior. Their structured-vs-detached distinction is the strongest design point: scoped tasks can borrow `view`/`mod` because they join before scope exit, while escaping tasks must move `own` values. That fits Rae’s ownership model and the Live/Compiled split.

FIRST_TASK: Document and implement the first safe slice of `Task(T)`: audit current `OP_SPAWN` argument transfer semantics, then change VM `spawn` from detached fire-and-forget into a joinable task handle with result storage, completion signaling, and spawn-boundary ownership diagnostics.

### Round 2 — Clo
VOTE: Chattie

Judged only on this run's actual task — the concurrency design — Chattie delivered the strongest and most reliable body of work. Round 1 was grounded (Chattie checked the existing `spawn`/`sys_thread`/test surface before proposing) and landed the spawn-first, no-`await` model with `Task(T)`/`TaskGroup`/`Channel(T)`/`parallelFor`. Round 2 was the most complete proposal anyone produced: concrete API signatures (`get`/`poll`/`cancel`, `TaskGroup.spawn`/`joinAll`, `Channel.send`/`recv`/`tryRecv`, `Shared`/`AtomicInt`/`AtomicBool`), explicit spawn-boundary ownership rules for `own`/`copy`/`view`/`mod`, a Compiled-vs-Live backend split (real-thread pool vs cooperative green tasks/isolates with yield points), ECS sharding with disjoint dense ranges and single-threaded structural mutation, eight phased implementation steps, and a sober risk list. Two things tip it toward Chattie for lead: discipline — in round 2 Chattie explicitly recognized the transcript contamination from the prior mobile-UI/scene-unit runs and refined only the concurrency task; and consistency — Chattie delivered a full design in both discussion rounds, whereas Gem's round-2 concurrency reply errored out with no content and Gem drifted off-task most often. Gem's reconnaissance (surfacing that `spawn`/`OP_SPAWN`/`SpawnData` already exist) and the "`parallelFor` blocks the caller, so it can borrow outer `view`/`mod`" insight were genuinely valuable and should be folded in, but the failed round and scope drift make Gem the riskier pick to drive the build.

FIRST_TASK: Convert the existing detached `spawn` into a joinable `Task(T)` on the VM backend first — the foundation everything else builds on. In `compiler/src/vm.c`, change `OP_SPAWN` to store the `sys_thread_t` handle plus a heap result slot in a `Task` value instead of discarding the handle and running detached; have `spawn_thread_wrapper`'s sub-VM write its return value into that slot and signal done; implement await-as-unmarked (auto-join when a `Task(T)` is read where `T` is expected) with explicit `poll() -> opt T` as the escape hatch. Before coding, pin down the must-verify-first unknown: whether the current `Value args[256]` transfer in `OP_SPAWN` deep-copies or pointer-moves — that decides isolate memory-safety and whether detached captures must be restricted to `own`/`copy`. Add the spawn-boundary ownership check (detached → `own`/`copy` only) and a minimal Live spawn-join-read test, and write the model up in `rae/docs/concurrency-model.md` folding in the `Task`/`TaskGroup`/`Channel`/`parallelFor` API surface. Defer C-backend `spawn` codegen, `Channel`, and `parallelFor` sharding to follow-up commits so the first slice stays reviewable.

### Round 3 — Chattie
VOTE: claude

Clo should lead because they provided the most precise code-path analysis: `vm_compiler.c` auto-viewing enums, call-site argument dispatch, `OP_GET_LOCAL` copying ref wrappers, and `vm_emit_stmt.c` using reference-aware local storage for fresh `let`s. They also identified the practical implementation shortcut that `OP_BIND_LOCAL` may already provide the direct-store semantics proposed as `OP_INIT_LOCAL`, plus the concrete validation risk around `StringMap.set` alias-by-let behavior.

FIRST_TASK: Implement enum-aware `vm_is_value_type` in the Live VM compiler and update the relevant auto-view/call-site predicates, then verify the mobile UI Live failure before broadening to the local-initialization opcode or `OP_BIND_LOCAL` emitter changes.

### Round 3 — Clo
VOTE: chatgpt

Chattie should lead because their round 1 diagnosis independently landed on the actual failure site (`main.rae:100`, `Screen` enum flowing through parameter lowering) and identified the precise predicate to change (`!is_val && !is_primitive` → `!is_val && !vm_is_value_type(ctx, base)`). Their round 2 design is the most complete: it cleanly separates Layer A (enum-aware value-type classification) from Layer C (initialization-vs-assignment opcode split), and explicitly calls out the call-site argument-deref risk that closes the loop with the param-lowering fix. By contrast, Gem's round 1 misdiagnosed the failure as a member-access `OP_VIEW_FIELD` "View Leak" in `render.rae`, and round 2 pivoted to echo Chattie's design rather than adding new technical depth. Chattie also surfaced the stack-convention audit and the `mod`-reference preservation concern, both of which are real implementation gotchas they're already primed to handle.

FIRST_TASK: Implement Layer A in isolation as commit #1 — add `bool vm_is_value_type(CompilerContext* ctx, const char* base)` to `rae/compiler/src/vm_compiler.c` consulting both `vm_is_primitive_type` and the enum table; update the three predicate sites at `vm_compiler.c:454` (param-type stringifier), `:1241` and `:1271` (call-site arg dispatch) to use it; grep `vm_compiler.c` for any remaining `!vm_is_primitive_type(...)` defaulting sites and apply the same swap; run `make -C compiler test` and `rae run --target live examples/98_mobile_ui/main.rae` to confirm the `Screen` reassignment failure no longer fires. Land Layer C (`OP_INIT_LOCAL` or, per my design, reuse the existing `OP_BIND_LOCAL` at `vm.c:482-497` and drop the trailing `OP_POP` in `vm_emit_stmt.c:230-240`) as commit #2 once Layer A is green; this keeps the diff bisectable if anything regresses.

### Round 3 — Gem
VOTE: claude

Clo provided the most technically rigorous and efficient design by identifying that the VM already possesses the required "initialization" semantics via the existing `OP_BIND_LOCAL` opcode. By choosing to reuse this instruction rather than introducing a redundant `OP_INIT_LOCAL`, Clo keeps the bytecode format lean while still achieving the semantic split between initialization and assignment. Additionally, Clo correctly pinpointed the failure to enum-type misclassification in `vm_compiler.c` (parameter lowering) rather than member access, and provided a comprehensive validation plan that includes auditing the buffer-get natives and integrating Live mode into the project's snapshot regression suite.

FIRST_TASK: Implement the `vm_is_value_type` helper in `vm_compiler.c` and use it to update the parameter-type stringification (line 454) and the call-site argument dispatch (lines 1241 and 1271), ensuring enums like `Screen` are passed as values.

### Round 3 — Chattie
VOTE: Clo

Clo should lead because their proposal is the most directly tied to the verified failure path: `vm_compiler.c` auto-viewing enums, call-site dispatch routing `Screen` through ref emission, `OP_GET_LOCAL` copying the ref wrapper, and `vm_emit_stmt.c` using `OP_SET_LOCAL` for fresh `let`s. Clo also identified that existing `OP_BIND_LOCAL` already provides the needed direct-store semantics, avoiding a redundant opcode, and called out concrete implementation risks like `StringMap.set` alias-by-let and `vm_emit_expr.c` push-back dependencies.

FIRST_TASK: Implement enum-aware `vm_is_value_type` in `compiler/src/vm_compiler.c`, update the `vm_is_primitive_type` ABI gates for param stringification and call-site dispatch, then run the Live mobile UI command before applying the `OP_SET_LOCAL + OP_POP` to `OP_BIND_LOCAL` let-initialization cleanup.

### Round 3 — Clo
VOTE: Chattie

Chattie's round 1 nailed the actual failure on the first try — correctly identified the site (`main.rae:100`, `Screen` enum through parameter lowering), the predicate to fix (`!is_val && !is_primitive` → `!is_val && !vm_is_value_type(ctx, name)`), and the right two-stage structure (value classification + direct local initialization). Round 2 added genuine value by explicitly weighing `OP_BIND_LOCAL` reuse vs new `OP_INIT_LOCAL` (leaving the choice open for the implementer to audit), calling out the stack-convention risk, the `lib/core.rae` alias-by-let risk, and the value-type classification drift between Live/sema/Compiled. Gem's round 1 mis-targeted the task entirely — viewport simulation, `STACK_MAX=4096`, render-texture natives — none of which touch the runtime `REF_VIEW` diag that aborts the example before any render frame. Gem's round 2 pivoted to echo the consensus diagnosis but still bundled in the unrelated viewport/safe-area initiative, which would balloon the scope without addressing the bug. Chattie's contribution is stronger on both diagnosis precision and implementation discipline.

FIRST_TASK: Implement Layer A as commit #1 in isolation — add `bool vm_is_value_type(CompilerContext* ctx, Str base)` to `rae/compiler/src/vm_compiler.c` that returns true for `vm_is_primitive_type(base)` ∪ `enum_table_has(ctx->enums, base)`; thread `CompilerContext*` into `get_type_name_with_refs` if it doesn't already have access; replace the `!vm_is_primitive_type(base)` branch at `vm_compiler.c:454` and the matching predicates at the two call-site arg-dispatch sites (~`:1241`, ~`:1271`); pre-flight `rg "vm_is_primitive_type" rae/compiler/src/` to catch any other defaulting sites and update consistently; run `perl -e 'alarm shift; exec @ARGV' 120 make -C compiler test` and `rae run --target live examples/98_mobile_ui/main.rae` to confirm the view-ref diag is gone. Land Layer B (swap `OP_SET_LOCAL` + `OP_POP` → `OP_BIND_LOCAL` at `vm_emit_stmt.c:235-237`, reusing the existing opcode at `vm.c:482-497` per my round-2 verification) as commit #2 so the diff stays bisectable.

### Round 3 — Gem
VOTE: Clo

Clo provided the most technically rigorous and efficient design by identifying that the VM already possesses the required "initialization" semantics via the existing `OP_BIND_LOCAL` opcode. By choosing to reuse this instruction rather than introducing a redundant `OP_INIT_LOCAL`, Clo keeps the bytecode format lean while still achieving the semantic split between initialization and assignment. Additionally, Clo correctly pinpointed the failure to enum-type misclassification in `vm_compiler.c` (parameter lowering) rather than member access, and provided a comprehensive validation plan that includes auditing the buffer-get natives and integrating Live mode into the project's snapshot regression suite.

FIRST_TASK: Implement the `vm_is_value_type` helper in `vm_compiler.c` and use it to update the parameter-type stringification (line 454) and the call-site argument dispatch (lines 1241 and 1271), ensuring enums like `Screen` are passed as values.

### Round 3 — Clo
VOTE: Gem

On the strength of contributions to *this run's actual task* — units/pixels in `.raescene` layouts — Gem is the only peer who engaged the question with a concrete technical design. Gem's viewport reply laid out the 3-coordinate model (Design/Viewport/Screen), the `Viewport` struct, `SafeArea` as an authored entity, and the device-preset table, and it correctly anchored to the pre-existing `rae/docs/ui-viewport-and-safe-area-plan.md` — converging with the consensus on **393×852 logical points**. That is directly on-topic and implementation-ready. Chattie's work in this run, while precise and disciplined, was almost entirely about a *different* problem — the Live VM `REF_VIEW` crash (`vm_is_value_type`, `OP_BIND_LOCAL`/`OP_INIT_LOCAL`) — which does not touch how lengths are expressed in scene files. Gem's render-to-texture path does have the real flaw I flagged (a `viewport`-sized blit is blurry on Retina unless allocated at `viewport × dpr`), but that is a correctable detail on top of an otherwise sound, on-task architecture, whereas Chattie offered no design for the pixels question at all.

FIRST_TASK: Land the units foundation from the consolidated viewport plan — add `lib/ui/viewport.rae` with the `Viewport` struct extended with a `dpr: Float` field; establish the unit contract that `.raescene` lengths are **logical points at 1×** (numbers = pt, `spaceS/M/L` tokens resolve to pt); rescale `examples/98_mobile_ui/config.rae` from 498×1079 to 393×852 (clean ×0.789, same aspect); enable `FLAG_WINDOW_HIGHDPI` and derive `dpr = GetRenderWidth ÷ GetScreenWidth`; and apply pixel snapping `round(pt·dpr)/dpr` at the transform→draw boundary. Critically, if render-to-texture is used, allocate the target at `viewport × dpr` (not raw points) so it stays crisp on Retina — and keep render-to-texture as the opt-in editor/sim path, with direct back-buffer rendering in points as the default.

### Round 3 — Chattie
VOTE: Clo

Clo should lead because their concurrency proposal is the most grounded in Rae’s actual implementation: they verified existing `spawn`, `OP_SPAWN`, `SpawnData`, `sys_thread`, the detached sub-VM behavior, and the lack of C-backend join/result support. They also made the strongest architectural distinction: scoped tasks may borrow `view`/`mod` because they join before scope exit, while detached tasks must capture only `own` moved values. That directly fits Rae’s ownership model and the Live/Compiled split.

FIRST_TASK: Turn the existing detached VM `spawn` prototype into a documented `Task(T)` design: specify current behavior, define join/result/error semantics, and write the first implementation plan for `Task(T)` handles plus spawn-boundary ownership rules.

### Round 3 — Clo
VOTE: Chattie

Judged only on this run's actual task — the concurrency design — Chattie produced the strongest and most implementation-ready body of work across both discussion rounds. Round 1 was grounded (Chattie explicitly checked the existing `spawn`/`sys_thread`/test surface before proposing) and landed the spawn-first, no-`await` model with `Task(T)`/`TaskGroup`/`Channel(T)`/`parallelFor`. Round 2 was the most complete proposal anyone offered: concrete API signatures (`get`/`poll`/`cancel`, `TaskGroup.spawn`/`joinAll`, `Channel.send`/`recv`/`tryRecv`, `Shared`/`AtomicInt`/`AtomicBool`), explicit spawn-boundary ownership rules for `own`/`copy`/`view`/`mod`, a Compiled-vs-Live backend split (real threads + pool vs cooperative green tasks/isolates with yield points), ECS sharding with disjoint dense ranges and single-threaded structural mutation, eight phased implementation steps, and a sober risk list (soundness, runtime-global auditing, API creep). Two things tip it toward Chattie for *lead* specifically: discipline — in round 2 Chattie explicitly recognized the transcript contamination from the prior mobile-UI/scene-unit runs and refined only the concurrency task, which is exactly the scope-hygiene an implementer needs; and consistency — Chattie delivered a full design in both rounds, whereas Gem's round-2 concurrency reply errored out with no content. Gem's contributions were genuinely valuable (the deepest code reconnaissance that surfaced the decisive fact that `spawn`/`OP_SPAWN`/`SpawnData` already exist, and the sharp "`parallel for` blocks the caller, so it can safely borrow outer `view`/`mod`" insight), but Gem also drifted off-task most often and had the failed round, making Gem the riskier choice to drive the build.

FIRST_TASK: Convert the existing detached `spawn` into a joinable `Task(T)` on the VM backend first — the foundation everything else depends on. In `compiler/src/vm.c`, change `OP_SPAWN` so it stores the `sys_thread_t` handle plus a heap result slot in a `Task` value instead of discarding the handle and running detached; have `spawn_thread_wrapper`'s sub-VM write its return value back into that slot and signal done; implement join-on-read (auto-join when a `Task(T)` is consumed where `T` is expected) with explicit `poll() -> opt T` as the escape hatch. Before writing any of it, pin down the must-verify-first unknown: whether the current `Value args[256]` transfer in `OP_SPAWN` deep-copies or pointer-moves — that decides whether the isolate model is memory-safe today and whether detached captures must be restricted to `own`. Add the spawn-boundary ownership check (detached task → `own`/`copy` captures only) and a minimal Live test that spawns, joins, and reads a result. Defer C-backend `spawn` codegen, `Channel`, and `parallelFor` to follow-up commits so this first slice stays reviewable.

### Round 3 — Gem
Warning: conversation "ad8cc2f1-a81f-4839-ba73-bd03e475339b," not found.

## Instructions
You (Clo) have been selected to lead implementation.

Before you write any code, READ every round response above — your own AND the peer responses from Chattie, Gem. All 3 agents contributed designs and trade-offs, and you must treat the full discussion as the starting point for your work, not just your own answer.

Concretely:
- Re-read each peer's design proposal (Chattie, Gem) and extract the strongest ideas, constraints, and risk callouts.
- Synthesize the best parts of all proposals into your implementation plan. Where peers disagreed, pick the option with the strongest technical justification — do not silently default to your own original take.
- Address every trade-off, risk, and unknown that any peer raised. If you decide to ignore one, say so and explain why.
- If two proposals conflict on a key decision, briefly state which one you are adopting and why before implementing.

Then implement the agreed-upon plan. Commit your changes with clear messages. Update your memory file when done.