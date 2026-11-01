# WASM modules: Rae code a host embeds

`rae build --target wasm` builds a **module** when the project's `.raepack`
has a `wasm` target with `exports`: a WASI reactor (no `main`, an
`_initialize` the host calls once) that exports the listed Rae functions by
their names, with the Rae runtime inside. Without `exports` the same command
builds the browser program it always did (Emscripten). SUMU's hosted apps are
such modules (its docs/HOSTED_APPS_PLAN.md §4).

```
pack Module {
  format: "raepack"
  version: 1
  defaultTarget: wasm
  targets: {
    target wasm: {
      label: "Module"
      entry: "Main.rae"
      exports: "add, greet"
      sources: { source: { path: "." emit: compiled } }
    }
  }
}
```

```sh
rae build --target wasm --project . --out build/module.wasm Main.rae
```

It is built with wasi-sdk (`$WASI_SDK`, default `~/.local/wasi-sdk`):
`clang --target=wasm32-wasip1 -mexec-model=reactor`. There is no
`--allow-undefined`: a symbol the code uses and nothing defines fails the
build instead of turning into a host import, so the module imports only WASI
(`wasi_snapshot_preview1`: what wasi-libc's start-up and the runtime use, the
clock, `fd_write`, the environment, `proc_exit`).

## The ABI

Each export is a wrapper the compiler writes (`compiler/src/c_module_exports.c`)
around the Rae function, exported under the function's name:

| Rae | WASM |
|---|---|
| `Int` | `i64` |
| `Float` / `Float64` | `f32` / `f64` |
| `Bool` | `i32` (0 / 1) |
| `view String` parameter | two `i32`: the address and byte length of UTF-8 the host wrote into the module's memory; read only during the call |
| `String` result | one `i64`: the address in the high 32 bits, the length in the low 32; the host reads the bytes, then frees the address |
| no result | nothing |

Two more exports: `raeModuleAlloc(size) -> address` (memory the host fills
with a String argument) and `raeModuleFree(address)` (for that memory and for
every String result). Every call first sets the runtime up once (what `main`
does for a program) and leaves the String pool as it found it. A String
parameter must be `view`; other types (structs, lists, `opt`) and `mod`
parameters are refused at build time, naming the export: typed interfaces
generated from the Rae types on both sides are Rae #129.

## The check

`compiler/tools/wasm-module-check.sh` (one pre-suite case of every full run,
`make wasm-module-check` alone) builds `compiler/tests/wasmModule` and runs
`host.mjs` under Node: only WASI imports, the exports present and no
`main`/`_start`, `add`, `greet` with a Unicode name and an empty result, 2000
calls in bounded memory; then a pack exporting a missing function and one
exporting a `List` parameter must fail with a diagnostic. It skips without
wasi-sdk or Node.

`compiler/tools/wasm_build.sh` still builds whole WASI programs (a `_start`
that runs `main`).
