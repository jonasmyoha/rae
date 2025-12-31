# Rae Interpreter & Bytecode Plan (T010)

## Goals
- Provide a production-ready interpreter that executes Rae source without recompilation to C.
- Enable hot-reload: compile a file to bytecode, swap the chunk at runtime, resume execution without restarting.
- Share the existing frontend (lexer/parser/AST) with the future C backend so semantic rules stay consistent.

## Architecture Overview
1. **Frontend** – reuse the current parser to produce `AstModule`.
2. **Bytecode compiler** – walk the AST and emit instructions into a `Chunk` structure (constant pool + code vector).
3. **Virtual machine** – stack-based interpreter executing opcodes (`PUSH_CONST`, `ADD`, `CALL`, etc.) with a small call frame structure.
4. **Runtime registry** – map module names to compiled chunks. Hot-reload replaces a chunk in the registry and resets any affected call frames.
5. **Embed API** – expose C functions so shipped binaries can call `rae_vm_compile(path)` and `rae_vm_execute(module, entry)`.

## Bytecode Format (v0)
- `Chunk`:
  - `uint8_t* code` – instruction bytes
  - `Value* constants` – tagged union (ints, strings)
  - `uint16_t* lines` – line info for diagnostics
- Instruction encoding: single-byte opcode + optional operands (little-endian). Example:
  - `0x01 PUSH_CONST <uint16 index>`
  - `0x02 ADD`
  - `0x03 PRINT`
  - `0x10 CALL <uint16 callee>`
- Initial value types: integers, booleans, strings.

## Hot-Reload Flow
1. Watcher (CLI/devtools) detects file change.
2. Recompile AST → Chunk.
3. Swap chunk in registry: `rae_vm_registry_replace(name, chunk)`.
4. If the module contains `main`, restart the main fiber; otherwise update call sites lazily (future optimization).

## Task Breakdown
1. **T011 – Bytecode compiler + VM**
   - Implement `Chunk`/`Value` structs, emit instructions for literals, arithmetic, `def`, and `log`/`logS`.
   - VM executes a single module via `bin/rae run <file>`.
   - Add `examples/hello.rae` that logs “Hello, Rae!”.
2. **T012 – Hot-reload + runtime API**
   - Add registry + watcher-friendly reload API, ensure CLI can run in watch mode.
   - Document embedding functions for shipped builds (bytecode compiler + VM available as a library).
3. **T013 – C backend plan/bootstrap** (from earlier roadmap) describing how the interpreter coexists with the transpiler.
4. **T014 – Devtools runner** hooking into `bin/rae run` for examples.

## Deliverables for T010
- This document in `docs/vm-plan.md`.
- Hub tasks T011–T014 queued (done).
- Approval to begin implementation (starting with T011).

## Hot-Reload Notes (T012)
- The CLI now exposes `bin/rae run --watch <file>` which recompiles the module whenever the source file changes and reruns it without restarting the process.
- `VmRegistry` keeps compiled chunks alive so embedders can replace or execute modules at will:
  - Load via `vm_registry_load(&registry, path, chunk)`
  - Swap with `vm_registry_reload` after recompiling
  - Execute by passing the stored `Chunk` to `vm_run`
- Embedding guidance: link against the VM library, call `vm_compile_module` + registry helpers to load scripts at runtime, and trigger reloads from your watcher or network layer.
