# Rae Live/Compiled Hybrid Execution Plan

## Goals
1. Allow a single project to mix Live (bytecode VM) Rae functions with Compiled code without maintaining two separate entry points.
2. Preserve the fast iteration loop of `rae run` (Live mode) while enabling Compiled-only features (FFI, raylib, etc.) in the same codebase.
3. Provide a predictable build/CLI story so contributors can target Live, Compiled, or Hybrid without guessing which pipeline to use.
4. Lay the groundwork for hot-reload / devtools features that need Live VM execution backed by host callbacks.

## Progress snapshot
- CLI already accepts `--target <live|compiled|hybrid>` and `--profile <dev|release>` so downstream tooling consumes a single flag surface.
- `VmRegistry` + `OP_NATIVE_CALL` allow Live bytecode to invoke registered externs while keeping the hot-reload watcher behavior unchanged.
- `rae build --target live` now walks the module graph once, emits a `.vmchunk` bytecode file, and writes the portable `*.manifest.json` beside it so hosts/devtools can inspect exported functions without rerunning the frontend.
- `rae build --target hybrid` materializes `<out>/vm/*` (Live chunk + manifest) and `<out>/compiled/*` (Compiled C + runtime) in one pass, so a `.hybrid` folder already behaves like the planned `.raepkg`.
- Compiled builds still pass `--emit-c`; future work focuses on manifest hand-off + host registration to wire devtools and embedders directly into hybrid bundles.

## 1. Shared Function Metadata

### Problem
The Live VM currently treats every `func` as a fully-defined Rae body. The Compiled pipeline now understands `extern func`, but there is no way for the Live VM to see that intent or to enable hybrid behavior.

### Plan
- Extend the AST + module metadata so every function carries a `FuncKind` enum: `Rae`, `Extern`, `NativeStub`.
- Introduce optional attributes (e.g., `@live_stub`, `@compiled_only`) that can appear before `func` to override defaults.
- Keep the parser tolerant: unknown attributes are ignored (so future evolution won’t break older compilers).
- Materialize a per-module “function manifest” (`module.funcs[]`), capturing:
  - symbol name
  - kind (Rae/Extern/NativeStub)
  - signature (params/returns)
  - (for extern/compiled) the expected host symbol or registry key.

This manifest will be serialized alongside Live bytecode / Compiled artifacts so both runtimes share the same view.

**Hot-reload & embedding considerations**
- Keep the manifest portable so host apps can ship the Rae compiler/VM and download new scripts at runtime.
- When embedding the compiler (see CLI), a Compiled host can compile → emit bytecode/manifest after launch and hot-reload without redeploying binaries.

## 2. Hybrid Build Pipeline

### Problem
`rae run` (Live mode) and `rae build --target compiled --emit-c` operate independently. Users must choose between them, and there is no single command that emits both bytecode + Compiled outputs in one go.
Hybrid bundles become the unit hot-reload hosts can fetch/replace so Live + Compiled apps extend themselves dynamically.

### Plan
- Introduce `rae build --target <live|compiled|hybrid>` (default: `hybrid`).
- In hybrid mode:
  1. Run the module graph once.
  2. Emit VM bytecode for all `FuncKind::Rae` functions.
  3. Emit C for all functions (as today) plus `extern` scaffolding.
  4. Serialize the function manifest (JSON or binary) to `build/<target>.manifest`.
  5. Package outputs into a `.raepkg` folder containing bytecode, manifest, generated C, runtime, and any host libs.
- `rae run` will be a thin wrapper around `rae build --target hybrid` followed by launching the Live VM using the latest bytecode/manifest; for pure Live projects the Compiled pieces are inert.

**Current layout**
`rae build --target hybrid --out path.hybrid` already emits:

```
path.hybrid/
  vm/<entry>.vmchunk
  vm/<entry>.manifest.json
  compiled/<entry>.c
  compiled/rae_runtime.{c,h}
```

This matches the `.raepkg` goal (minus future metadata/config files) so tooling can rely on a stable directory structure today.

## 3. VM FFI Bridge

### Problem
The Live VM cannot call `extern func` today. We need a way to bind external/Compiled functions so the bytecode interpreter can invoke them when running hybrid builds.

### Plan
- Add an FFI registry in the VM runtime (`vm_registry.c/h`):
  ```c
  typedef int64_t (*RaeNativeFn)(VM* vm, const NativeCall* call);
  void vm_register_native(const char* name, RaeNativeFn fn);
  ```
- During startup, load host bindings (e.g., the devtools server would register `tinyexpr_eval`, raylib wrappers, etc.).
- When compiling bytecode:
  - For `FuncKind::Extern`, instead of emitting a body, emit a stub instruction (`OP_NATIVE_CALL <name>`).
  - During VM execution, `OP_NATIVE_CALL` looks up the function in the registry and marshals parameters/returns using the manifest signature.
- Provide a default registry file (`vm_registry_builtin.c`) so core functions (log/logS) keep working, and allow hosts to extend it (devtools server, CLI, etc.).
- Hosts that ship the compiler can expose a limited `vm_register_native` surface so downloaded Rae scripts call into host systems safely.

## 4. CLI & Profiles

### Problem
Users currently have to remember which command to run. We need a consistent UX around “Live”, “Compiled”, and “Hybrid” modes, plus dev/release profiles that toggle hot-reload vs optimized builds.

### Plan
- Add `rae build --profile <dev|release>` and `--target <live|compiled|hybrid>` flags.
- Map the existing commands:
  - `rae run foo.rae` => `rae build --target hybrid --profile dev foo.rae` followed by launching the Live VM on the emitted bytecode.
  - `rae build --emit-c` => deprecated alias for `rae build --target compiled --emit-c`.
- Surface the chosen profile in devtools so the dashboard can show whether it’s running Live, Compiled, or Hybrid.
- Document the defaults: `rae run` chooses `hybrid+dev`, `rae build` chooses `compiled+release` unless overridden.
- Provide `--embed-compiler` (opt-in) so release builds can include the Rae compiler/Live VM for runtime script downloads; default release builds can skip it to save space.
- Reserve `--target live-release` for future pure-Live (web/WASM) deployments even if implementation arrives later.

## Deliverables
1. AST/manifests encode function kinds + signatures.
2. CLI accepts `--target`/`--profile`, saved in `docs/cli.md`.
3. Live VM gains an FFI registry + `OP_NATIVE_CALL`.
4. `rae build --target hybrid` emits bytecode + manifest + generated C in one pass.
5. Devtools and docs updated with the new workflow.
