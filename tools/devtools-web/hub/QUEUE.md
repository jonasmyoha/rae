# Rae hub — Task Queue

## Rules
- Tasks are small (30–90 min)
- Each task has Acceptance checks
- Results go to hub/RESULTS/T###.md

## Tasks

### T009 - Compiler: research minimal C graphics library for game example
- Repo: rae
- Summary:
  - Evaluate lightweight, high-quality C drawing/3D libs suitable for embedding in Rae runtime examples (e.g., raylib, sokol_gfx, NanoVG)
  - Compare licensing, build complexity, and integration steps for macOS/Linux
  - Recommend one library and outline how to create a Pong/Tetris demo using it once multifile support lands
- Acceptance:
  - Research notes committed under `docs/` covering pros/cons and setup steps for the recommended library
  - Include next actions for building the Pong prototype (toolchain commands, assets, etc.)
