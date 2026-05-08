# LLM Iteration Plan

## Goal

Maximize LLM iteration speed and reduce "C init order / unused warning / brittle global state" failures. Prioritize rapid compile/test cycles and fewer agent-stalling errors.

## Phase 1: Reduce Warning-as-Error Friction

- [x] Implement `MODE=DEV_FAST` in `Makefile`.
- [x] Disable "stalling" errors (`unused-variable`, `unused-parameter`, `unused-function`) in `DEV_FAST`.
- [x] Keep critical errors (`implicit-function-declaration`, `incompatible-pointer-types`, `format`) as errors.
- [x] Add `tools/dev_fast.sh` as the primary agent command.

## Phase 2: Eliminate Init-Order Footguns

### Intent

Refactor project-wide state (singletons, static arrays) into a unified `CompilerContext`.

### Files

- `src/main.c`
- `src/c_backend.c`
- `src/vm_compiler.c`

### Target

```c
typedef struct {
    StringInterner* interner;
    DiagState* diags;
    TypeTable* types;
    SymbolTable* symbols;
    Arena* ast_arena;
    Arena* backend_arena;
    // ...
} CompilerContext;
````

### Tasks

* [x] Refactor project-wide state (singletons, static arrays) into a unified `CompilerContext`.

  * [x] Define `CompilerContext` in `ast.h`.
  * [x] Update `c_backend.h` signature.
  * [x] Refactor `c_backend.c` to use `ctx->` instead of `g_` globals.
  * [x] Update `main.c` to initialize and pass `CompilerContext` for C builds.
  * [x] Update `vm_compiler.c` to use `CompilerContext`.
  * [ ] Unify `StringInterner` and `TypeTable` into `CompilerContext`.

* [ ] Ban implicit "lazy init" via `static bool initialized`. Use explicit `compiler_init(ctx)`.

## Phase 3: Make Code Patterns LLM-Proof

* [ ] Arena-Based Lifetimes: Prefer `arena_alloc` over manual `free` for AST and IR nodes to reduce lifetime errors.
* [>Gemini] Centralized Mangler: Move all mangling logic out of `c_backend.c` into a shared `src/mangler.c`.
* [ ] ID-Based Comparisons: Replace `if (str_eq(type_name, "Buffer"))` with `if (type->id == CORE_TYPE_BUFFER)`.
* [ ] Assertion Helpers: Add `RAE_ASSERT(condition, message)` that prints context-aware diagnostics before failing.

## Phase 4: Fast Smoke Testing

* [x] Target handful of files + one C backend compile.
* [ ] Integration with `git pre-commit` if requested.

## Phase 5: Architectural File Splitting

**Goal:** Break down context-killing "God Files" into smaller, domain-specific modules with stable APIs.

* [ ] Refactor `c_backend.c` (~4.6k LOC) into domain-specific files (`c_types.c`, `c_expr.c`, etc.).
* [ ] Refactor `main.c` (~3.7k LOC) into `cli.c`, `driver.c`, `vm_natives_core.c`.
* [ ] Refactor `vm_compiler.c` (~3k LOC) into `vm_emit_expr.c`, `vm_emit_stmt.c`.

### 1. Refactor `c_backend.c` (~4.6k LOC)

Split into:

* `c_types.c`: Struct emission and monomorphisation registration.
* `c_expr.c`: `emit_expr` switch and expression helpers.
* `c_stmt.c`: `emit_stmt`, `emit_block`, and control flow.
* `c_intrinsics.c`: Compiler-intrinsic logic (e.g., `__buf_get`).
* `c_runtime_raylib.c`: Third-party integration wrappers.
* `c_runtime_json.c`: JSON method generation.

### 2. Refactor `main.c` (~3.7k LOC)

Split into:

* `cli.c`: Command-line argument parsing and help.
* `driver.c`: Orchestration of the module graph and build process.
* `vm_natives_core.c`: Core native function registration.

### 3. Refactor `vm_compiler.c` (~3k LOC)

Split into:

* `vm_emit_expr.c`: Bytecode generation for expressions.
* `vm_emit_stmt.c`: Bytecode generation for statements and control flow.

### 4. Mandatory Rules for Splitting

* **Stable API:** Every split must introduce a clean `Context` struct (e.g., `CBackendContext`) and corresponding headers.
* **No Globals:** Strictly ban modules from poking random globals across files.
* **No Junk Drawers:** Avoid "glue" or "misc" files. If a file exists, it must have a clear "Why" (e.g., `c_runtime_json.c` vs `c_runtime_glue.c`).

## Sequencing Note

1. **DEV_FAST / Warnings mode first** (DONE).
2. **Architectural File Splitting** (to establish stable APIs).
3. **Monomorphisation** (deeper semantic changes).

```
```
