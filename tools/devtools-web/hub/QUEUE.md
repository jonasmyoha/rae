# Rae hub — Task Queue

## Rules
- Tasks are small (30–90 min)
- Each task has Acceptance checks
- Results go to hub/RESULTS/T###.md

## Tasks

### T007 - Devtools: unify test run + code viewer UI
- Repo: rae-devtools-web
- Summary:
  - When selecting a test in the UI, open both its latest results and source code in a shared view (tabs or split pane)
  - Provide quick navigation between test output, source, and metadata (command, expectations)
  - Ensure the interaction works for tests that have not run yet (display placeholder state)
- Acceptance:
  - Selecting any test shows an integrated view combining run status/output and code without needing separate panels
  - Works in `bun run dev`, and basic keyboard/mouse navigation behaves as expected

### T008 - Compiler: plan multifile build support
- Repo: rae
- Summary:
  - Document the concrete steps required to compile and link a simple multi-file Rae program (module boundaries, build graph, CLI flags)
  - Identify parser/AST/runtime gaps blocking multi-file compilation and propose minimal changes
  - Outline a small demo program that would validate the feature once implemented
- Acceptance:
  - Design/plan document checked into `docs/` (or `spec/`) describing the technical approach and milestones
  - Actionable list of follow-up engineering tasks with rough effort estimates

### T009 - Compiler: research minimal C graphics library for game example
- Repo: rae
- Summary:
  - Evaluate lightweight, high-quality C drawing/3D libs suitable for embedding in Rae runtime examples (e.g., raylib, sokol_gfx, NanoVG)
  - Compare licensing, build complexity, and integration steps for macOS/Linux
  - Recommend one library and outline how to create a Pong/Tetris demo using it once multifile support lands
- Acceptance:
  - Research notes committed under `docs/` covering pros/cons and setup steps for the recommended library
  - Include next actions for building the Pong prototype (toolchain commands, assets, etc.)
