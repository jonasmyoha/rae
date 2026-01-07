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

### Status note — Hybrid devtools work in progress
- Repo: `rae-devtools-web` / `rae`
- Summary:
  - Multi-target (Live/Compiled/Hybrid) config + UI landed; example runner now captures hybrid artifacts with hashes.
  - Added `examples/hybrid_hot_reload` plus `devtools.json` metadata to drive the hybrid-only example card.
  - Simulate-download helper buttons now call the script with version folders (`version1/2/3`) to stage `.vmchunk` files under `.simulated_downloads/<profile>/<version>/`, compiled/hybrid dashboard test targets run the slim regression subsets via `TARGET=compiled|hybrid`, and the Examples panel lists the staged bundles + hashes under “Staged downloads.”
  - Remaining items for next session: tie staged downloads into a future “simulate host reload” indicator and continue filling any hybrid tooling gaps discovered during user testing.
