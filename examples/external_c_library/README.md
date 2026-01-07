# Tinyexpr Rae Demo

This example shows how Rae code can call into the single-file [tinyexpr](https://github.com/codeplea/tinyexpr) library via the C backend.

## Building
1. Ensure `third_party/tinyexpr/tinyexpr.c` and `.h` exist (run `tools/ffi/install_tinyexpr.sh` if needed).
2. Emit C from Rae (repo root):
   ```bash
   compiler/bin/rae build --emit-c examples/tinyexpr-demo/main.rae --out build/tinyexpr_demo.c
   ```
   This also drops `rae_runtime.{c,h}` beside the emitted source.
3. Compile/link with the runtime + tinyexpr wrapper:
   ```bash
   cc -Ibuild -Ithird_party/tinyexpr \
      build/tinyexpr_demo.c \
      third_party/tinyexpr/rae_tinyexpr.c \
      third_party/tinyexpr/tinyexpr.c \
      build/rae_runtime.c \
      -o build/tinyexpr_demo
   ```
4. Run:
   ```bash
   ./build/tinyexpr_demo
   # Result:
   # Result:
   # 14
   ```
   Alternatively, from this directory run `make demo` to perform steps 2–4 automatically (requires the compiler to be built already).

## Notes
`tests.rae` documents the expressions we expect tinyexpr to evaluate; it will become runnable once the Rae test harness understands `test` blocks.
