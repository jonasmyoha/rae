# Rae VM/Native Hybrid Execution Plan

## Goals
1. Allow a single project to mix VM-executed Rae functions with extern/native code without maintaining two separate entry points.
2. Preserve the fast iteration loop of `rae run` (VM) while enabling native-only features (FFI, raylib, etc.) in the same codebase.
3. Provide a predictable build/cli story so contributors can target VM, native, or both, without guessing which pipeline to use.
4. Lay the groundwork for hot-reload / devtools features that need live VM execution backed by host callbacks.

## 1. Shared Function Metadata

### Problem
The VM currently treats every `func` as a fully-defined Rae body. The native pipeline now understands `extern func`, but there is no way for the VM to see that intent or to enable hybrid behavior.

### Plan
- Extend the AST + module metadata so every function carries a `FuncKind` enum: `Rae`, `Extern`, `NativeStub`.
- Introduce optional attributes (e.g., `@vm_stub`, `@native_only`) that can appear before `func` to override defaults.
- Keep the parser tolerant: unknown attributes are ignored (so future evolution won’t break older compilers).
- Materialize a per-module “function manifest” (`module.funcs[]`), capturing:
  - symbol name
  - kind (Rae/Extern/NativeStub)
  - signature (params/returns)
  - (for extern/native) the expected host symbol or registry key.

This manifest will be serialized alongside bytecode/native artifacts so both runtimes share the same view.

**Hot-reload & embedding considerations**
- Keep the manifest portable so host apps can ship the Rae compiler/VM and download new scripts at runtime.
- When embedding the compiler (see CLI), a native app can compile → emit bytecode/manifest after launch and hot-reload without redeploying native binaries.

## 2. Hybrid Build Pipeline

### Problem
`rae run` (VM) and `rae build --emit-c` (native) operate independently. Users must choose between them, and there is no single command that emits both bytecode + native outputs in one go.
Hybrid bundles become the unit hot-reload hosts can fetch/replace so native apps extend themselves dynamically.

### Plan
- Introduce `rae build --target <vm|native|hybrid>` (default: `hybrid`).
- In hybrid mode:
  1. Run the module graph once.
  2. Emit VM bytecode for all `FuncKind::Rae` functions.
  3. Emit C for all functions (as today) plus `extern` scaffolding.
  4. Serialize the function manifest (JSON or binary) to `build/<target>.manifest`.
  5. Package outputs into a `.raepkg` folder containing bytecode, manifest, generated C, runtime, and any host libs.
- `rae run` will be a thin wrapper around `rae build --target hybrid` followed by launching the VM using the latest bytecode/manifest; for pure VM projects the native pieces are inert.

## 3. VM FFI Bridge

### Problem
The VM cannot call `extern func` today. We need a way to bind external/native functions so the bytecode interpreter can invoke them when running hybrid builds.

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
- Hosts that ship the compiler can expose a limited `vm_register_native` surface so downloaded Rae scripts call into native systems safely.

## 4. CLI & Profiles

### Problem
Users currently have to remember which command to run. We need a consistent UX around “VM”, “native”, and “hybrid” modes, plus dev/release profiles that toggle hot-reload vs optimized builds.

### Plan
- Add `rae build --profile <dev|release>` and `--target <vm|native|hybrid>` flags.
- Map the existing commands:
  - `rae run foo.rae` => `rae build --target hybrid --profile dev foo.rae` followed by launching the VM on the emitted bytecode.
  - `rae build --emit-c` => deprecated alias for `rae build --target native`.
- Surface the chosen profile in devtools so the dashboard can show whether it’s running VM, native, or hybrid.
- Document the defaults: `rae run` chooses `hybrid+dev`, `rae build` chooses `native+release` unless overridden.
- Provide `--embed-compiler` (opt-in) so release builds can include the Rae compiler/VM for runtime script downloads; default release builds can skip it to save space.
- Reserve `--target vm-release` for future pure-VM (web/WASM) deployments even if implementation arrives later.

## Deliverables
1. AST/manifests encode function kinds + signatures.
2. CLI accepts `--target`/`--profile`, saved in `docs/cli.md`.
3. VM gains an FFI registry + `OP_NATIVE_CALL`.
4. `rae build --target hybrid` emits bytecode + manifest + generated C in one pass.
5. Devtools and docs updated with the new workflow.
