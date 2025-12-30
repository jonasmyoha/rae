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
