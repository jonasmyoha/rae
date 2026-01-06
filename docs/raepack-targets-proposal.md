# Proposal: .raepack Targets + Devtools Multi-Target Runs

## Summary
This proposal defines a `.raepack` format that declares arbitrary build targets (e.g., `live`, `compiled`, `hybrid`, or `ios`) and splits sources by emit mode (`live`, `compiled`, or `both`). Devtools should stop using a global target dropdown and instead run builds/tests/examples per target defined by `.raepack` (or defaults when absent).

The goal is to make Live/Compiled/Hybrid an explicit, per-package choice rather than a global UI toggle, while still keeping the current `rae` CLI surface (`--target`, `--profile`) intact.

## Goals
- Let packages define multiple targets with explicit source splits.
- Run tests/examples against all applicable targets by default.
- Keep the CLI single-binary (`bin/rae`) with target selection driven by `.raepack`.
- Make devtools actions target-aware without requiring global dropdowns.

## Definitions
- **Target**: A named build configuration inside `.raepack`. Names are arbitrary.
- **Emit mode**: `live`, `compiled`, or `both` for each source path.
- **Live**: Bytecode VM output (`.vmchunk` + manifest).
- **Compiled**: C output + runtime.
- **Hybrid**: A target that emits both Live + Compiled outputs according to source splits.

## Proposed `.raepack` schema (v1)
Data-only Rae syntax (no code, no `data {}` wrapper, `{}` blocks only).

```rae
pack MyApp: {
  format: "raepack"
  version: 1
  defaultTarget: live
  targets: {
    target live: {
      label: "Live"
      entry: "src/main.rae"
      sources: {
        source: { path: "src", emit: live }
      }
    }
    target compiled: {
      label: "Compiled"
      entry: "src/main.rae"
      sources: {
        source: { path: "src", emit: compiled }
      }
    }
    target hybrid: {
      label: "Hybrid"
      entry: "src/host/main.rae"
      sources: {
        source: { path: "src/host", emit: compiled }
        source: { path: "src/scripts", emit: live }
        source: { path: "src/shared", emit: both }
      }
    }
  }
}
```

### Rules
- Top-level must be `pack <Name>: { ... }`.
- Required fields: `format` (string), `version` (int), `defaultTarget` (ident), `targets` (block).
- `targets` contains one or more `target <id>: { ... }` entries.
- Each target requires: `label` (string), `entry` (string), `sources` (block).
- `sources` contains one or more `source: { path: string, emit: ident }` entries.
- `emit` must be one of: `live | compiled | both`.
- `entry` must resolve to a file included by the resolved `sources` set (folders recursive).
- Unknown fields are allowed and preserved for forward compatibility.
- Repeated keys represent lists (e.g., multiple `source:` entries).

## Build semantics
For a given target:
1. Resolve the source set from `sources`.
2. Split into `live`, `compiled`, and `both`.
3. Compile:
   - **Live**: emit `.vmchunk` + manifest for `live` + `both`.
   - **Compiled**: emit C + runtime for `compiled` + `both`.
4. Emit a target manifest that lists:
   - exports per module
   - emit mode per module
   - entry file

Outputs should follow the existing layout for hybrid builds:

```
<out>/
  vm/<entry>.vmchunk
  vm/<entry>.manifest.json
  compiled/<entry>.c
  compiled/rae_runtime.{c,h}
  <target>.manifest.json
```

## Runtime bridging (proposal)
We need both directions:
- **Live → Compiled**: already possible via `extern func` + `OP_NATIVE_CALL`.
- **Compiled → Live**: add a runtime API to call VM functions by name.

### Proposed runtime API (C)
- `rae_live_load(vm, path)` – load a `.vmchunk` and register it.
- `rae_live_call(vm, name, args...)` – call a Live function by symbol name.
- `rae_live_list(vm)` – list Live exports for debugging.

These are host-facing C APIs. Later we can add Rae stdlib wrappers, e.g.
`live.load(path)` and `live.call(name, args)` for compiled hosts that embed the VM.

## Devtools changes (proposal)
### Remove global target dropdown
Targets should be resolved per test/example:
- If `.raepack` exists, enumerate its targets.
- If absent, use default targets (`live`, `compiled`) for tests and examples.

### Build/Clean/Rebuild
Because there is one compiler binary, a single build is usually sufficient. If `.raepack`
targets map to different commands, devtools can run each target sequentially and de-duplicate
identical commands.

### Tests
- Run tests sequentially per target (Live then Compiled by default).
- Record output grouped by target label.
- Update `compiler/tools/run_tests.sh` to skip tests that do not support a target.
- Add optional per-test `.raepack` (or `NNN_test.raepack`) to override supported targets.

### Examples
- If `.raepack` exists, render per-target buttons (e.g., **Run Live**, **Run Compiled**).
- If absent, default to Live + Compiled.
- `devtools.json` remains for UI metadata; supported targets should come from `.raepack`.

## CLI changes (proposal)
- `rae build --target <id>` reads `.raepack` and selects the target.
- `rae run` uses `defaultTarget` if defined.
- `rae build --list-targets` prints available target IDs/labels.

## Migration steps
1. **Devtools**: remove dropdown, add per-target buttons, run tests sequentially per target.
2. **Compiler**: add `.raepack` parser + target resolution.
3. **Tests**: support `.raepack` per test to declare target availability.
4. **Hybrid runtime**: add `rae_live_*` APIs and a minimal example using them.
5. **Docs**: align `docs/live-compiled-hybrid-plan.md` with `.raepack` targets.

## Open questions
- Should targets without explicit `label` default to sentence-case of the ID?
- How do we version `.raepack` format?
- How do we express “compiled-only entry calling live code” without new language syntax?

## Notes on conflicts with current plans
This proposal generalizes targets beyond fixed `live|compiled|hybrid`. It will require
aligning or replacing the assumptions in `docs/live-compiled-hybrid-plan.md` once approved.
