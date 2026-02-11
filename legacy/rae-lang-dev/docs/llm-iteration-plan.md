# Plan: LLM Iteration & Compiler Stability

## Goal
Maximize LLM iteration speed and reduce "C init order / unused warning / brittle global state" failures. Prioritize rapid compile/test cycles and fewer agent-stalling errors.

## Phase 1: Reduce Warning-as-Error Friction
- [x] Implement `MODE=DEV_FAST` in `Makefile`.
- [x] Disable "stalling" errors (`unused-variable`, `unused-parameter`, `unused-function`) in `DEV_FAST`.
- [x] Keep critical errors (`implicit-function-declaration`, `incompatible-pointer-types`, `format`) as errors.
- [x] Add `tools/smoke.sh` for < 5s sanity checks.
- [x] Add `tools/dev_fast.sh` as the primary agent command.

## Phase 2: Eliminate Init-Order Footguns
- [>Gemini] Refactor project-wide state (singletons, static arrays) into a unified `CompilerContext`.
  - [x] Define `CompilerContext` in `ast.h`.
  - [x] Update `c_backend.h` signature.
  - [x] Refactor `c_backend.c` to use `ctx->` instead of `g_` globals.
  - [x] Update `main.c` to initialize and pass `CompilerContext` for C builds.
  - [ ] Update `vm_compiler.c` to use `CompilerContext`.
  - [ ] Unify `StringInterner` and `TypeTable` into `CompilerContext`.
- [ ] Ban implicit "lazy init" via `static bool initialized`. Use explicit `compiler_init(ctx)`.

## Phase 3: Make Code Patterns LLM-Proof
- [ ] Arena-Based Lifetimes: Prefer `arena_alloc` over manual `free` for AST and IR nodes.
- [ ] Centralized Mangler: Move all mangling logic out of `c_backend.c` into a shared `src/mangler.c`.
- [ ] ID-Based Comparisons: Replace `if (str_eq(type_name, "Buffer"))` with `if (type->id == CORE_TYPE_BUFFER)`.
- [ ] Assertion Helpers: Add `RAE_ASSERT(condition, message)` that prints context-aware diagnostics.

## Phase 4: Fast Smoke Testing
- [x] Target handful of files + one C backend compile.
- [ ] Integration with `git pre-commit` if requested.

## Phase 5: Architectural File Splitting
- [ ] Refactor `c_backend.c` (~4.6k LOC) into domain-specific files (`c_types.c`, `c_expr.c`, etc.).
- [ ] Refactor `main.c` (~3.7k LOC) into `cli.c`, `driver.c`, `vm_natives_core.c`.
- [ ] Refactor `vm_compiler.c` (~3k LOC) into `vm_emit_expr.c`, `vm_emit_stmt.c`.

## Sequencing Note
1. **DEV_FAST / Warnings mode first** (DONE).
2. **Architectural File Splitting** (Establishing stable APIs).
3. **Monomorphisation** (Deeper semantic changes).