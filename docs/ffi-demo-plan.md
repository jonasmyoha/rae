# Rae FFI Bootstrap Plan — Tinyexpr Demo

## Objectives
- Prove that Rae-generated C can link against an off-the-shelf C library before the more complex raylib integration.
- Keep the footprint tiny so contributors can reproduce the flow without extra build systems or OS-specific SDKs.
- Document every step (download, verification, build, Rae example, and automation hooks) so this becomes the canonical template for future FFI demos.

## Selected Library: `tinyexpr`
| Trait | Details |
|-------|---------|
| Purpose | Single-file math expression evaluator written in ISO C |
| License | Zlib (fully permissive, suits Rae’s redistributable examples) |
| Files | `tinyexpr.c` + `tinyexpr.h` (≈1.6k LOC combined) |
| Build | Compile the `.c` file and link it with the generated Rae C output |
| Rationale | No external deps, deterministic behavior, easy to validate via numeric tests |

## Download & Verification
1. Add a helper script `tools/ffi/install_tinyexpr.sh` that:
   - accepts an optional destination (defaults to `third_party/tinyexpr`);
   - downloads pinned versions of `tinyexpr.c/.h` from https://github.com/codeplea/tinyexpr;
   - validates the SHA256 checksums;
   - drops a short README alongside the sources.
2. Run the script once to stage the baseline third-party snapshot (kept small enough for git).

## Integration Sketch
1. **Wrapper header** (temporary): declare `extern "C"` functions Rae code will call.
   ```c
   // third_party/tinyexpr/rae_tinyexpr.h
   #pragma once
   double rae_eval_expr(const char* expr);
   ```
2. **Wrapper implementation** converts Rae strings to `const char*` and uses `tinyexpr_parse` / `te_eval`.
3. **Rae declaration file**:
   ```rae
   // lib/tinyexpr.rae
   extern func eval(expr: string) -> number
   ```
4. **Example project** (`examples/tinyexpr-demo/`) with:
   - `main.rae` – prompts the user, calls `tinyexpr.eval`.
   - `tinyexpr_test.rae` – asserts `eval("2+3*4") == 14`.

## Build Flow
1. `bin/rae build --emit-c examples/tinyexpr-demo/main.rae --out build/tinyexpr-demo.c`
   - Alternatively, run `make -C examples/tinyexpr-demo demo` to emit C, copy the runtime, and invoke `cc`.
2. Compile+link with the wrapper + library:
   ```bash
   cc -Ithird_party/tinyexpr \
      build/tinyexpr-demo.c \
      third_party/tinyexpr/rae_tinyexpr.c \
      third_party/tinyexpr/tinyexpr.c \
      -o build/tinyexpr-demo
   ```
3. Run smoke test: `./build/tinyexpr-demo <<< "2+2"`.

## Test Strategy
- Add a Rae unit-test file (`examples/tinyexpr-demo/tests.rae`) that exercises a handful of expressions.
- Mirror the same inputs in a native C test (optional) to ensure wrapper parity.
- Add a regression test under `compiler/tests/cases/406_build_extern.*` that asserts the generated C includes `extern` prototypes and string parameters.
- Hook into CI later by compiling the example with `make tinyexpr-demo`.

## Next Steps
1. Land `tools/ffi/install_tinyexpr.sh` + `third_party/tinyexpr/README.md`.
2. Add the wrapper C files + Rae `extern` declarations.
3. Scaffold the demo project + tests.
4. Automate build/test commands via a `Makefile` target so the devtools dashboard can fire it once the Pong work arrives.
