# Rae Compiler

C11 implementation of the Rae compiler (Phase 1: parse and pretty-print).

## Building

```bash
make build
```

## Testing

```bash
make test
```

## Usage

```bash
bin/rae lex <file>     # tokenize
bin/rae parse <file>   # parse + dump AST
bin/rae format <file>  # pretty-print canonical Rae (stdout by default)
bin/rae run <file>     # execute Rae source via the bytecode VM (hot reload path)
bin/rae build --target hybrid --out out.hybrid <file>  # emit bundled Live+Compiled assets (.raepkg-style dir)
bin/rae build --target live --out out.vmchunk <file>   # emit Live (bytecode VM) artifact + function manifest
bin/rae build --emit-c --out out.c <file>  # transpile tiny Rae programs to C
```

Pass `--watch` to keep the VM alive and reload the file whenever it changes:

```bash
bin/rae run --watch examples/hello.rae
```

Watch mode now monitors every module pulled in via imports or auto-import folders. Try it on
the auto-import demo to see helper files reloaded without explicit imports:

```bash
bin/rae run --watch examples/auto_import_demo/main.rae
```

Early C backend support can turn simple `log`-centric, arithmetic-only programs into compilable C.
Right now every parameter is treated as an `int64_t`, returns are not yet supported, and `log` only
accepts string literals, but it is handy for tiny demos:

```bash
bin/rae build --emit-c --out build/demo.c examples/c_backend_demo.rae
```

`rae build --target live` walks the same module graph but emits a portable `.vmchunk` file alongside
`*.manifest.json`, giving tooling and hosts a consumable artifact without going through the C
transpiler:

```bash
bin/rae build --target live --out build/demo.vmchunk examples/hello.rae
```

`rae build --target hybrid` packages both outputs (Live bytecode + Compiled C/runtime) under a single
directory (named with `.hybrid` by convention) so devtools can drop-in load VM code while the native
side embeds the transpiled files without rebuilding on demand:

```bash
bin/rae build --target hybrid --out build/demo.hybrid examples/hello.rae
ls build/demo.hybrid
# vm/bytecode.vmchunk, vm/bytecode.manifest.json, compiled/<entry>.c, compiled/rae_runtime.{c,h}
```

Formatting options:

```bash
bin/rae format --write file.rae             # overwrite the input in place
bin/rae format --output tmp.pretty file.rae # write to a different file
```

Only one of `--write/-w` or `--output` may be supplied per invocation. If
neither flag is passed the formatter simply streams the canonical code to
stdout, which keeps shell pipelines simple.

See `../spec/rae.md` for language specification.
