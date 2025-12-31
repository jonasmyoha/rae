# Rae Multifile Build Plan (T008)

> **Status:** `bin/rae run` now resolves both absolute (`"compiler/modules/foo"`) and relative (`"./ui/header"`, `"../shared/time"`) imports, and when a file contains no explicit imports the CLI automatically scans its directory tree for `.rae` files and includes them. The remaining tasks below focus on richer semantics and the `rae build` pipeline.

## Goals
- Compile and bundle a Rae application that spans multiple `.rae` files without requiring manual concatenation.
- Keep Phase 1 constraints (lex/parse/pretty) but sketch the hand-off to future semantic passes so the plan stays valid.
- Produce a concrete demo outline that exercises cross-file calls (e.g., `App.rae` importing `gfx/renderer.rae` + `game/logic.rae`).

## Current State & Gaps
1. **CLI** – `bin/rae` supports `lex|parse|format <file>` only. No way to specify a project root, dependency graph, or output artifact.
2. **Parser/AST** – files parse independently; there is no notion of modules or exports/imports. Every declaration lives in a single compilation unit.
3. **Build tooling** – `make build` just compiles the C compiler. There is no `rae build` task to orchestrate multi-file analysis.
4. **Examples/tests** – All fixtures live under `compiler/tests/cases/*.rae`. No sample project that proves multi-file wiring.

## Proposed Architecture

### 1. Project layout + metadata
- Adopt a simple layout now to minimize future churn:
  ```
  app/
    app.raepack         # optional metadata (name, auto folders, targets)
    src/
      main.rae
      render/ui.rae
      state/store.rae
  ```
- `.raepack` files are first-class Rae syntax documents. They live at the project root and can describe branding (display name, bundle identifier), `auto_folders`, dependencies, and build targets. Code may reference `package.name`, `package.brand.name`, etc., so renames happen in one place.
- If no `.raepack` is present and the folder only contains a single `.rae` entry point, the CLI assumes the entire folder (and subdirectories) form the package automatically. This keeps the learn-to-code experience zero-config, while larger apps drop in a `.raepack`.
- Document conventions (module path mirrors relative file path, `.` vs `/` mapping) in `docs/` + `spec/rae.md`.

### 2. Module graph loader
- Add a lightweight loader in C that:
  1. Walks the source tree collecting `.rae` files.
  2. Parses each file into an `AstModule` using existing parser.
  3. Records module metadata: file path, declared types/funcs, dependencies (initially implicit via `spawn Foo()` or `Foo()` calls – explicit `import` syntax can arrive later).
- Store this in a new `ModuleGraph` struct so future passes (name resolution, ownership checking) can operate across files.

### 3. CLI experience
- Introduce `bin/rae build [--project <dir>] [--entry <file>] [--out <path>]`.
- Reuse the current arena/lexer/parser pipeline per file; aggregate diagnostics before exiting.
- Short-term output can be:
  - `--emit ast`: dump merged AST for inspection.
  - `--emit pretty`: write formatted sources to `build/` to mimic a bundle.
- Long-term this command will feed semantic analysis + codegen.

### 4. Demo program
- Target: **Pong-style** program with three files:
  1. `main.rae` – handles window init + game loop stub.
  2. `game/logic.rae` – updates paddle/ball positions (pure logic).
  3. `render/simple.rae` – prints text-mode output for now; later swaps to graphics backend (ties into T009).
- Document build steps in `examples/multifile-pong/README.md` so contributors can run:
  ```
  cd examples/multifile-pong
  ../../bin/rae build --project .
  ```

## Follow-up Engineering Tasks (rough order & sizing)
| Task | Description | Est. Effort |
|------|-------------|-------------|
| T010 | Implement `ModuleGraph` + filesystem walker | 1–2 days |
| T011 | Extend CLI with `rae build` options + merged diagnostics | 1 day |
| T012 | Wire pretty-printer to emit combined bundle (per-file + aggregated) | 0.5 day |
| T013 | Author `examples/multifile-pong` skeleton + docs | 0.5 day |
| T014 | (Stretch) Add explicit `import Foo.bar` syntax + parser support | 2–3 days |
| T015 | Hook devtools build controls to `rae build` for multi-file projects | 0.5 day |

## Risks & Mitigations
- **Missing import syntax** – Start with implicit linking by compiling all files together; document limitations so early adopters understand ordering rules.
- **Large project performance** – Loader can reuse the arena allocator per file but should share a top-level arena for graph metadata to avoid leaks.
- **Future semantic passes** – Keep module graph neutral (file path + exported decl list) so later phases can add symbol tables without rewriting the loader.

## Deliverables for T008
1. This plan checked into `docs/multifile-plan.md`.
2. `README.md` / `spec/rae.md` references pointing to the plan (future task).
3. Ticket backlog (T010–T015) ready to be queued in the hub when implementation begins.
