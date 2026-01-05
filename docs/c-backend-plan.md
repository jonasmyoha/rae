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

#### Front-End Parity Implementation
1. `module_graph_build()` becomes the single choke point for both `rae run` and `rae build`. Each call hashes file contents so hot reload + multi-target builds share cache keys.
2. The interpreter immediately merges the graph (`merge_module_graph`) while the compiled path keeps both the merged tree (for manifests) and the raw module list for per-file emission.
3. The ModuleGraph stores the resolved entry module path, so diagnostics in compiled mode can still reference logical names even when emitting multiple `.c` files.
4. Watch lists (`watch_sources_collect`) already fuel `rae run --watch`; the same API feeds `rae build --emit-c --watch` later without teaching two components how to traverse the filesystem.
5. Any new semantic analysis pass must live above ModuleGraph so both runtimes run exactly the same validation steps before codegen.

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

### 3a. Stub Output While Codegen Matures
- Phase 0 still writes a `.c` file so downstream scripts can inspect something tangible even though the backend is unfinished.
- The stub lists every parsed Rae function, emits a `main` that prints a roadmap pointer, and returns non-zero so CI remembers it is not production ready.
- The CLI still writes the manifest JSON based on the merged AST, which lets devtools confirm the frontend path is wired correctly without executing the emitted C.
- Copying runtime headers is skipped for the stub to avoid confusing users into thinking full codegen happened; the minute real emission succeeds, runtime assets and manifests mirror the final layout.

### 4. Milestones
| Milestone | Description | Deliverable |
|-----------|-------------|-------------|
| M1 | CLI plumbing + scaffolding | `rae build --emit-c <file>` validates args via ModuleGraph, then writes the stub `.c` output + manifest so downstream tooling can smoke-test the flow. |
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
- When the actual emitter is not ready, drop a stub file that references `docs/c-backend-plan.md` so humans + automation get a deterministic artifact instead of a missing file error.

## Shared Runtime Considerations
- Logging: wrap `fprintf(stdout, ...)` + `fflush` to match VM behavior.
- Strings: maintain a small struct with pointer + length so future Unicode work has a home.
- Memory: expose `rae_alloc/rae_free` hooks so embedders can override allocation.
- Hot reload: keep runtime stateless; devtools can rebuild/relink without holding global state.
- Stub mode skips copying the runtime to keep its intent obvious, but the manifest file is still emitted from the merged AST so tooling can verify which functions were parsed.

## Acceptance Criteria for T013
1. This plan checked into `docs/c-backend-plan.md`.
2. CLI stub `rae build --emit-c <file>` exists, validates args, and exits with a TODO message.
3. No functional codegen yet, but the command acts as the anchor for upcoming milestones.
