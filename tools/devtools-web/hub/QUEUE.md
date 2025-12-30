# Rae hub — Task Queue

## Rules
- Tasks are small (30–90 min)
- Each task has Acceptance checks
- Results go to hub/RESULTS/T###.md

## Tasks

### T010 - Design interpreter + bytecode pipeline
- Repo: rae
- Summary:
  - Document the interpreter architecture (bytecode format, VM ops, loader API) and how it coexists with the future C transpiler
  - Outline hot-reload flow (compile Rae -> bytecode chunk -> swap into running VM)
  - Capture required runtime support for embedding in shipped builds (bytecode compiler + VM available in release builds)
- Acceptance:
  - Design doc checked into `docs/` describing VM opcodes, chunk layout, and APIs
  - Includes task breakdown for implementation (T011+)

### T011 - Implement minimal Rae bytecode VM
- Repo: rae
- Summary:
  - Add a bytecode compiler that lowers expressions/statements to a simple instruction set
  - Implement a VM loop that can execute arithmetic, assignments, and `print`
  - Gate behind `bin/rae run --vm` for now with a single-file hello world example
- Acceptance:
  - `rae/compiler` builds the VM, `bin/rae run examples/hello.rae` prints “Hello, Rae!”
  - Basic tests cover bytecode emission + VM stack behavior

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

### T014 - Devtools Example Runner (build/run source tabs)
- Repo: rae-devtools-web
- Summary:
  - Add an “Examples” panel listing Rae example projects (single-file now, multi-file later)
  - Each entry offers Build/Run buttons that call `bin/rae build/run`; output and source show in tabbed view similar to tests
  - Prepare data model for multi-file projects (list of files, entry point, command metadata)
- Acceptance:
  - Running the hello world example via devtools triggers the VM and streams output
  - UI shows example source tabs and handles multi-file metadata even if only one file exists today
