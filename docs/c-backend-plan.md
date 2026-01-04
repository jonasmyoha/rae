# Rae Compiler C Backend Plan (T013)

## Objectives
- Deliver a concrete roadmap for the first Compiled code backend that lowers Rae programs into portable C.
- Ensure the new backend reuses the existing front-end (lexer/parser/AST) so Live + Compiled stay in sync.
- Provide a CLI surface area (`rae build --target compiled --emit-c`) that eventually emits runnable C artifacts, even if the first milestone is a stub.

## Current Baseline
1. `bin/rae` supports `lex|parse|format|run` only; there is no `build` command or notion of targets.
2. VM bytecode executes a single `func main` body with basic literals and `log`/`logS`.
3. No module graph or semantic analysis beyond syntax validation.

## Proposed Architecture

### 1. Shared Front-End
- Reuse the existing parser/AST for every source file.
- Introduce a `ModuleGraph` loader (from T008/T010) that enumerates `.rae` files, builds ASTs, and records exported declarations.
- Feed the same AST nodes to both the VM compiler and the new C backend so feature parity stays guaranteed.

#### Module Ordering & Graph Behavior
- Entry module always resolves first, then the graph walks imports breadth-first so emitted files have deterministic ordering.
- Graph nodes keep both module path (`foo/bar`) and real file path so future tooling can show precise diagnostics.
- Interpreter + compiler share this loader; interpreter still merges modules into one AST while the compiler emits per-module units.
- Auto-import logic (single-file scripts) lives in the graph so both execution modes infer the same implicit imports.

### 2. Intermediate Representation
- Stage 1: operate directly on AST nodes (expression statements only). Emit inline C for literals, string constants, and `log`/`logS`.
- Stage 2: add a normalized IR (SSA-lite) to support control flow, locals, and ownership checks before codegen.
- Keep IR structures in `compiler/src/ir/*.c` so later backends (LLVM, WASM) can reuse them.

### 3. Code Emission Strategy
- Generate one `.c` file per Rae module; include a shared runtime header (`rae_runtime.h`) that defines `rae_string`, logging helpers, and allocation APIs.
- Provide a `runtime/` directory with:
  - `rae_runtime.h` – type defs, macros for `log`/`logS`.
  - `rae_runtime.c` – minimal implementations (string literals, logging wrappers, event loop stubs).
- The CLI should copy these runtime files beside the emitted C or reference them via `#include "rae_runtime.h"`.
- Main compilation pipeline:
  1. Parse modules → AST.
  2. (Future) Run semantic passes (type/ownership).
  3. Emit C:
     - map Rae strings to `const char*`.
     - `log` → `rae_log(value)` (adds newline), `logS` → `rae_log_stream(value)`.
     - Functions become `static` C functions with mangled names (`rae_mod_main()`).
  4. Generate a driver `main.c` that calls `rae_mod_main()` inside `int main()`.
  5. Emit a manifest JSON alongside the artifacts so future toolchains (devtools web) can pick them up without re-parsing the CLI output.

### 4. Milestones
| Milestone | Description | Deliverable |
|-----------|-------------|-------------|
| M1 | CLI plumbing + scaffolding | `rae build --emit-c <file>` prints “Not implemented” but parses options and validates input, touching ModuleGraph to guarantee the frontend path. |
| M2 | Single-module emitter | Produce `build/out.c` with literals, `log`/`logS`, and `func main`. Includes runtime header. |
| M3 | Multi-module support | Leverage ModuleGraph to emit multiple C files + driver. |
| M4 | Runtime polish | Flesh out `rae_runtime.[ch]` (string helpers, memory API, platform shims). |
| M5 | Tooling integration | Devtools dashboard gains a “Build (C)” button invoking the new CLI path. |

## CLI Design (`rae build`)

```
bin/rae build [--emit-c] [--project <dir>] [--entry <file>] [--out <path>]
```
- **Phase 0 (this task):** accept `--emit-c` flag and entry file; fail with a descriptive “C backend not implemented yet” message.
- Future phases expand the flag set to choose IR dumps (`--emit-ir`), specify target OS, and toggle optimizations.

The long-term goal is for `rae build` to double as the package manager: a folder defines a package by default (no manifest required), but an optional `.raepack` file can describe multiple targets (desktop/mobile), release/debug profiles, and publishable versions. If a `.raepack` file exists in the directory, it overrides the automatic “walk every subfolder” behavior so explicit manifests can constrain what gets built. The CLI should understand "`build everything under this folder`" without requiring users to list every file.

### Option Parsing Sketch
```text
BuildOptions {
  const char* entry_path;
  const char* out_path;
  bool emit_c;
}
```
- Require `--emit-c` for now to avoid ambiguous behavior.
- Default `out_path` to `build/out.c`.
- Validate that `entry_path` (positional arg) exists before continuing.

## Shared Runtime Considerations
- Logging: wrap `fprintf(stdout, ...)` + `fflush` to match VM behavior.
- Strings: maintain a small struct with pointer + length so future Unicode work has a home.
- Memory: expose `rae_alloc/rae_free` hooks so embedders can override allocation.
- Hot reload: keep runtime stateless; devtools can rebuild/relink without holding global state.

## Acceptance Criteria for T013
1. This plan checked into `docs/c-backend-plan.md`.
2. CLI stub `rae build --emit-c <file>` exists, validates args, and exits with a TODO message.
3. No functional codegen yet, but the command acts as the anchor for upcoming milestones.
