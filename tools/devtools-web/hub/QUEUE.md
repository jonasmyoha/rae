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

### Status note — Hybrid devtools work in progress
- Repo: `rae-devtools-web` / `rae`
- Summary:
  - Multi-target (Live/Compiled/Hybrid) config + UI landed; example runner now captures hybrid artifacts with hashes.
  - Added `examples/hybrid_hot_reload` plus `devtools.json` metadata to drive the hybrid-only example card.
  - Remaining items for next session: hook up “simulate downloaded bundle” helper + finalize dedicated compiled/hybrid test subsets.
