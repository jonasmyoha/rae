- Only announce important events using `SAY: <message>` lines. Speak nothing else.

# Rae hub — Task Queue

## Rules
- Tasks are small (30–90 min)
- Each task has Acceptance checks
- Results go to hub/RESULTS/T###.md

## Tasks

### T013 - Compiler C backend plan & bootstrap
- Repo: rae
- Summary:
  - Draft the codegen plan (module ordering, C emission strategy, runtime stubs) and scaffold a `rae build` command that emits C for expressions + functions
  - Ensure the plan explains how interpreter + compiler share the frontend
- Acceptance:
  - Plan checked into `docs/` with milestones
  - CLI stub (`rae build --emit-c`) exists even if output is minimal

### T014 - Main loop test (short-run)
- Repo: rae
- Summary:
  - Add a deterministic main loop test that runs 5 ticks and prints tick output.
  - Keep output stable for regression comparisons.
- Acceptance:
  - New test case under `compiler/tests/cases/` runs for 5 ticks.
  - `.expect` output is deterministic and checks tick numbering.

### T015 - Main loop example (indefinite, stop button)
- Repo: rae + rae-devtools-web
- Summary:
  - Add/adjust a main loop example that runs indefinitely and logs tick numbers.
  - Ensure the devtools "Stop" button can terminate the running loop.
- Acceptance:
  - Example runs indefinitely in devtools and stops cleanly via "Stop".
  - Example logs ticks in a readable, consistent format.

### T016 - Hybrid hot-reload main loop example (dev profile)
- Repo: rae + rae-devtools-web
- Summary:
  - Extend the hybrid hot-reload demo with a main loop that calls Live bytecode.
  - Demonstrate hot-reload of Live code while the compiled host keeps running.
- Acceptance:
  - Example shows a running loop, and hot-reloaded Live functions visibly change behavior.
  - Devtools UI exposes a clear action to trigger the hot-reload flow.

### T017 - Hybrid hot-reload example (release-style downloads)
- Repo: rae + rae-devtools-web
- Summary:
  - Simulate downloaded versioned bundles and swap Live code at runtime.
  - Use the versioned folders to show code replacement behavior.
- Acceptance:
  - Example uses versioned downloads (`version1/2/3`) and hot-reloads them.
  - Devtools UI lists staged downloads and signals reload events.

### T018 - .raepack parser tests (error cases)
- Repo: rae
- Summary:
  - Add negative tests for missing fields, invalid emit, bad entry, and trailing tokens.
  - Keep error output deterministic.
- Acceptance:
  - New `.expect` files cover error output for each case.
  - Tests fail with a single diagnostic line per case.

### T019 - .raepack CLI target resolution (stub)
- Repo: rae
- Summary:
  - Wire `.raepack` parsing into CLI surface so targets can be inspected and validated.
  - Add a machine-friendly `rae pack` output for devtools.
- Acceptance:
  - `rae pack` validates `.raepack` and can emit JSON.
  - Errors are clear for unknown targets or invalid packs.

### T020 - .raepack example scaffold
- Repo: rae
- Summary:
  - Add a minimal example package that ships a `.raepack` and entry file.
  - Document the targets in a short README.
- Acceptance:
  - Example passes `rae pack` validation.
  - Devtools can surface it as an example.

### T021 - Devtools multi-target buttons (no dropdown)
- Repo: rae-devtools-web
- Summary:
  - Remove the global target dropdown.
  - Render per-target buttons per example/test based on `.raepack` (fallback to live/compiled).
- Acceptance:
  - Run tests executes Live then Compiled sequentially.
  - Examples show separate Run buttons for each supported target.

### T022 - Fix devtools comment syntax highlighting
- Repo: rae-devtools-web
- Summary:
  - Investigate and fix the issue where comments starting with `#` display a partial `tok-comment">` tag in the syntax highlighter.
  - Ensure comments are correctly highlighted without rendering HTML tags.
- Acceptance:
  - Comments in code viewer appear correctly highlighted.
  - No stray HTML tags visible in source view.

### T023 - Refactor Raylib C wrappers to PascalCase
- Repo: rae
- Summary:
  - Rename C wrapper functions in `rae/third_party/raylib/rae_raylib.c` from `snake_case` (e.g., `rae_raylib_init_window`) to `PascalCase` (e.g., `InitWindow`).
  - Keep the prefix in C to avoid collisions but allow clean mapping if possible, or decide on a consistent naming strategy that allows `InitWindow` in Rae.
  - For now, since we wrap them, we can name the C wrappers `Rae_InitWindow` or similar, but the goal is to expose `InitWindow` to Rae.
  - Wait, if we use `extern func InitWindow` in Rae, the C backend emits `InitWindow`. This conflicts with Raylib's `InitWindow`.
  - So we MUST use a prefix in C backend output or in the wrapper.
  - Strategy: Rename wrappers to `Rae_InitWindow`. In `raylib.rae` (T024), we will bind `extern "Rae_InitWindow" func InitWindow` if alias is supported, OR we just use `func InitWindow(...) { return Rae_InitWindow(...) }` wrapper in Rae.
  - Actually, let's just rename the C functions to `Rae_InitWindow` (PascalCase with prefix) to match Raylib style better, or just `InitWindow` if we can avoid linking issues (we can't).
  - Let's stick to: C wrappers named `Rae_InitWindow`.
- Acceptance:
  - `rae_raylib.c` uses `PascalCase` for function names (with prefix).
  - Wrapper logic remains correct.

### T024 - Centralize Raylib wrapper in raylib.rae
- Repo: rae
- Summary:
  - Create `rae/lib/raylib.rae` (or similar) containing `extern func` definitions for Raylib.
  - expose clean `InitWindow`, `BeginDrawing` names to user code.
  - Update `pong.rae` and `raylib_basic.rae` to import this file and use clean names.
- Acceptance:
  - `rae/lib/raylib.rae` exists.
  - Examples import it and use `InitWindow` etc.
  - `pong` and `raylib_basic` compile and run.

### T025 - Reorder examples
- Repo: rae-devtools-web
- Summary:
  - Change example ordering in `src/server/examples.ts`.
  - `raylib_basic` -> 12, `pong` -> 13.
- Acceptance:
  - Devtools list shows `raylib_basic` before `pong`.

### Status note — Hybrid devtools work in progress
- Repo: `rae-devtools-web` / `rae`
- Summary:
  - Multi-target (Live/Compiled/Hybrid) config + UI landed; example runner now captures hybrid artifacts with hashes.
  - Added `examples/hybrid_hot_reload` plus `devtools.json` metadata to drive the hybrid-only example card.
  - Simulate-download helper buttons now call the script with version folders (`version1/2/3`) to stage `.vmchunk` files under `.simulated_downloads/<profile>/<version>/`, compiled/hybrid dashboard test targets run the slim regression subsets via `TARGET=compiled|hybrid`, and the Examples panel lists the staged bundles + hashes under “Staged downloads.”
  - Remaining items for next session: tie staged downloads into a future “simulate host reload” indicator and continue filling any hybrid tooling gaps discovered during user testing.
