# Ownership transfer semantics for auto-drop

Status: **design — to be implemented by Codex**.
Tracking: Stage 3 of `docs/scope-exit-dealloc.md`.
Tests blocking Stage 2 auto-drop re-enablement:
`411_json_parser`, `412_ui_ecs`, `413_scene_loader`,
`414_ui_layout`, `416_memory_leak_check`.

## The problem

We need to design and implement Rae ownership transfer semantics
for auto-drop, **without breaking Rae's core rule**:

- `=` means copy / value semantics.
- `=>` means bind a `view` / `mod` reference.
- Plain `T` parameters are `val T`, meaning copied by value.
- Ownership transfer must be explicit in syntax, **except** when
  calling a function whose parameter explicitly says `own T`.

Stage 2 added auto-drop for heap-owning locals. It fixed the
transient `List` leak test (`416`), but broke real tests via
double-free. Example:

```rae
func attachChildren(world: mod UiWorld, parent: Int, kids: view List(Int)) {
  let copy: List(Int) = createList(Int)(initialCap: kids.length)
  ...
  let ch: Children = { ids: copy }
  componentSet(this: world.childrens, entity: parent, data: ch)
}
```

The compiler auto-drops `copy` at scope exit, but `Children.ids`
points to the same backing buffer after a shallow C struct copy,
so the world later has a dangling pointer.

## Language decision

**Do NOT silently move** in ordinary assignments or struct field
init. These should copy, or fail if no valid deep copy exists:

```rae
let b: List(Int) = a
let ch: Children = { ids: a }
```

That means `let ch: Children = { ids: copy }` must NOT move
`copy`.

## Explicit ownership transfer with `own`

Recognise the `own` keyword in expression positions:

```rae
let ch: Children = { ids: own copy }
```

Meaning:

- `copy` is consumed / moved into `ch.ids`.
- `copy` must be marked moved.
- `copy` must not be auto-dropped at scope exit.
- Using `copy` after this should become a compiler error eventually.

## Owned parameters

Recognise `own T` as a parameter mode:

```rae
func componentSet(T)(this: mod ComponentTable(T), entity: Int, data: own T)
```

Meaning:

- The function takes ownership of `data`.
- At the call site, if the argument is a local owned value,
  passing it to this parameter moves it automatically:

  ```rae
  componentSet(..., data: ch)
  ```

  Because the callee parameter says `own T`, `ch` is consumed /
  moved.
- `ch` must not be auto-dropped at the caller scope exit.
- Plain `data: T` remains `data: val T`, copied by value.

## Keyword model

| Keyword | Meaning |
|---|---|
| `view T` | borrowed read-only |
| `mod T`  | borrowed mutable |
| `val T`  | copied value |
| `own T`  | ownership transfer / consuming parameter |

Plain `T` is sugar for `val T`.

## Function-call rule

| Param declared | Behaviour |
|---|---|
| `view T` | borrow, no move, no copy / ownership transfer |
| `mod T`  | mutable borrow, no move, no copy / ownership transfer |
| `val T` / plain `T` | **copy**. For heap-owning types this requires real copy semantics or should eventually error if no copy implementation exists. Do NOT shallow-copy owning values. |
| `own T`  | **move / consume** from caller into callee. Mark caller local moved. |

## Struct-constructor field rule

- `field: x`     means copy.
- `field: own x` means move / consume into the field.
- No implicit field move.

## Return rule

- Returning an owned local from a function moves it out:
  `ret x` (or `return x` depending on current syntax).
- The returned local must not be auto-dropped in the function.
- This is allowed because returning owned locals would otherwise
  be painful.

## Compiler implementation

### 1. AST support for `own` in expression positions

At minimum for struct field values:

```rae
{ ids: own copy }
```

Add an `AST_EXPR_OWN` / `AST_EXPR_MOVE` wrapper around the inner
expression (or similar).

### 2. Type / parameter support for `own T`

Existing parameter modes probably already distinguish `view` and
`mod`; extend with `val` / plain and `own`.

### 3. Moved-local tracking in `CFuncContext`

```c
bool local_moved[256];
```

### 4. Auto-drop emission rules

When emitting auto-drops:

- Skip params.
- Skip `view` / `mod` borrows.
- Skip locals where `local_moved[i] == true`.
- Drop remaining heap-owning locals in **LIFO** order.

### 5. Move detection — what NOT to do

Move detection must **NOT** scan all identifiers and mark them
moved. That would be wrong. Example:

```rae
let len: Int = copy.length
```

…must not move `copy`.

Only mark moved in **ownership-consuming contexts**:

- `own x` expression.
- argument `x` passed to a parameter declared `own T`.
- returning owned local `ret x`.
- maybe later assignment into an `own` destination, but not
  ordinary `=`.

### 6. For `own x` expression

- Emit the value of `x` as usual.
- Mark local `x` moved.
- Ensure the moved local is not dropped later.
- Eventually reject use-after-move.

### 7. For function calls

- Resolve the callee parameter mode.
- If parameter is `own T` and argument is a local owned value,
  mark that local moved.
- If parameter is `val T`, do not mark moved. It is a copy.
- If parameter is `view` or `mod`, do not mark moved.

### 8. For return statements

- If returning an owned local, mark it moved so auto-drop does not
  free it before / after return.

### 9. Update `componentSet`

Change this:

```rae
func componentSet(T)(this: mod ComponentTable(T), entity: Int, data: T)
```

to:

```rae
func componentSet(T)(this: mod ComponentTable(T), entity: Int, data: own T)
```

Because `componentSet` stores the component into the table and
should take ownership.

### 10. Inside `componentSet`, be careful with replacing existing data

Current code uses raw buffer set:

```rae
rae_ext_rae_buf_set(buf: datas.data, index: i, value: data)
```

For owning `T`, overwriting an existing slot must eventually drop
the old value before replacing it. If existing APIs do not support
that yet, document it clearly and/or add a safer `setOwned` path
later. For now, focus on preventing caller-scope double-free.

### 11. Update `attachChildren` to use explicit field ownership transfer

```rae
let ch: Children = { ids: own copy }
componentSet(this: world.childrens, entity: parent, data: ch)
```

Because `componentSet` has `data: own T`, the second line should
move `ch` automatically.

## Tests to write

**A. Minimal move-into-field test:**

- Create a `List(Int)`.
- Put it into a struct field using `{ ids: own list }`.
- Verify list contents are accessible through the struct.
- Verify no double-free / no crash with auto-drop enabled.

**B. Same test without `own`:**

- `{ ids: list }` should either deep-copy correctly or fail if
  deep-copy is not implemented.
- It must not silently move.
- If current compiler cannot error yet, add an expected-fail test
  documenting the intended behaviour.

**C. Move into `own` parameter:**

- Function `consumeList(xs: own List(Int))`.
- Call it with a local list.
- Verify caller does not auto-drop the moved list.

**D. Plain `T` parameter still copies:**

- Function `takesVal(xs: List(Int))` or generic `data: T`.
- Passing an owning local must not mark it moved.
- If deep copy for `List` is unavailable, this should eventually
  become a compile error rather than a shallow copy.

**E. Return owned local:**

- Function creates a List and returns it.
- Caller can use it.
- Callee does not auto-drop returned storage.

**F. Regression tests:**

- `411_json_parser`
- `412_ui_ecs`
- `413_scene_loader`
- `414_ui_layout`
- `416_memory_leak_check` (the leak marker / transient List
  allocation RSS test — should flip from FAIL to PASS once
  auto-drop is safely re-enabled).

## Examples to add

- A small ownership example showing `val`, `view`, `mod`, and
  `own`.
- A `componentSet` example showing why it uses `own T`.
- A struct field example showing:
  - `{ ids: list }`     = copy
  - `{ ids: own list }` = transfer ownership

## Main goal

Re-enable auto-drop safely by making the compiler aware of
explicit ownership transfer. Preserve Rae's readability rule:

- `=`   copies
- `=>`  borrows
- `own` transfers ownership

## What's already in place for Codex to build on

- Stage 1 stdlib `drop()` API for `List(T)` / `StringMap(V)` /
  `IntMap(V)` in `lib/core.rae`.
- Stage 2 helpers:
  - `is_drop_target_type(type)`
  - `find_drop_overload_for(ctx, base)`
  - `emit_implicit_drops_for_body(ctx, out, first_let_index)`
- Discovery-pass pre-registration of `drop()` specialisations
  in `compiler/src/c_discovery.c` so the specialised symbol
  lands before its first caller.
- Block-scope tracking in `emit_if` / `emit_loop`
  (`ctx->local_count` save/restore).
- Two commented-out hook points in
  `compiler/src/c_backend.c::emit_function` and
  `emit_specialized_function` where the auto-drop call goes once
  Stage 3 is ready.

Search for `Stage 2` / `Stage 3` in `c_backend.c`, `c_stmt.c`,
and `c_discovery.c` to find every relevant location.

## Parameter-mode rules (current, normative)

These rules describe the state of the language as of Stage 5 of
the memory-safety overhaul. They are enforced unconditionally by
sema — no env flag, no opt-in.

- **Function params must be explicit**: `view T`, `mod T`,
  `copy T`, or `own T`. Choose the weakest mode that fits.
- **Bare `T` params are no longer accepted. Hard error since
  Stage 5.** The diagnostic names the parameter and the type
  and suggests all four explicit modes. Primitives
  (`Int`/`Int8…64`/`UInt…`/`Float…`/`Bool`/`Char`/`Char32`),
  enums, and `Any` are still allowed bare because their value
  semantics are unambiguous; externs are also skipped because
  their parameter lists describe FFI shape, not Rae ownership.
- **The legacy `val T` syntax is also a hard error** with a
  "use `copy T` instead" suggestion. `val` was deprecated when
  `copy` landed in Stage 3.
- **`view String` is the correct mode for read-only string
  parsing APIs.** Examples: `parseJson(source: view String)`,
  `parseScene(source: view String)`, `sceneNodeIndex(nodeId:
  view String)`, `applyComponentByName(name: view String)`. Use
  `own String` only if the function stores the string in a
  struct it owns.
- **Method-call receivers must preserve ownership / view
  classification.** Method-call `let` initializers MUST NOT be
  classified as owning by default: `let x: String =
  obj.list.get(index: i)` aliases the slot, so auto-dropping `x`
  at scope end would free the slot's data. The compiler now
  body-inspects the resolved method via `rae_func_returns_alias`
  to decide owning vs aliasing, the same way it already handles
  ident-call initializers.
- **View-T rvalue call args must use stable named temps,** not
  fragile compound-literal wrappers. The old
  `(rae_View_T){ .ptr = (T[]){expr} }` form had a torn-read
  pathology under nested calls in the same enclosing block.
  Codegen now hoists each view-T prim_wrap arg into a named
  stack temp inside a statement expression that wraps the
  entire call, so each arg has a distinct stack slot whose
  lifetime matches the SE block.

### Final verification numbers (2026-05-25, Stage 5)

- Unit tests: **310 passed, 0 failed** (includes new
  `480_bare_T_param_rejected` and `481_bare_val_param_rejected`
  negative tests asserting the new sema error)
- Example smoke tests: **50 passed, 0 failed**
- Bare-T / `val T` warnings anywhere in the codebase: **0**
  (the language no longer accepts them, so we no longer rely on
  a strict env flag)
- Leak tests `43[1,3,4,5,6,7,9]` + `440`: green under
  `RAE_MEM_STATS=1`, 0 outstanding allocations on exit
- Mobile UI 20k-iteration stress: RSS plateau at ~6.0 MB (97%
  reduction vs pre-Stage-3 baseline); no linear leak observed
