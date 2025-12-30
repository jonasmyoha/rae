# Rae hub — Task Queue

## Rules
- Tasks are small (30–90 min)
- Each task has Acceptance checks
- Results go to hub/RESULTS/T###.md

## Tasks

### T012 - Hot-reload + dev runtime support
- Repo: rae
- Summary:
  - Extend the VM with a chunk registry so functions/modules can be swapped without restarting the process
  - Expose CLI/server hook to reload a file on change (watch mode) and document the runtime API for embedding (e.g., `rae_vm_reload(path)`)
  - Prepare for release builds by ensuring bytecode compiler is available as a library
- Acceptance:
  - Manual demo showing a running VM session reloading a modified file
  - Docs describe embedding APIs and shipped runtime expectations

### T013 - Compiler C backend plan & bootstrap
- Repo: rae
- Summary:
  - Draft the codegen plan (module ordering, C emission strategy, runtime stubs) and scaffold a `rae build` command that emits C for expressions + functions
  - Ensure the plan explains how interpreter + compiler share the frontend
- Acceptance:
  - Plan checked into `docs/` with milestones
  - CLI stub (`rae build --emit-c`) exists even if output is minimal
