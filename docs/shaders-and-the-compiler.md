# Shaders and the compiler — declared compositions, build-time validation, hot reload

## The problem this solves

A WGSL shader is a string to the Rae compiler today. `lib/GbufferTerrain.rae`,
`lib/GrassCompute.rae` and eleven other lib modules (plus five runtime C files)
read `.wgsl` files **from disk at run time** (`gbReadWgsl` →
`rae_ext_rae_gb_read_shader`) and concatenate them — noise + biome + palette +
detail + splat — into the text handed to `wgpuDeviceCreateShaderModule`. The
first thing that ever parses that text is wgpu, on the GPU device, inside the
running app. So:

- An undefined identifier in a shader is invisible until the app runs, and then
  it is a Rust panic inside `libwgpu_native.dylib` (wgpu-native is written in
  Rust; `src/lib.rs:422` in such a backtrace is wgpu's file, not ours) that
  aborts the process. On 2026-09-19 a lib change added a call to
  `raeBiomeSample` in `lib/grass_compute.wgsl`; the repo's examples composed it
  with a `world_biome.wgsl` that defines it, a sibling game project did not, and
  that project crashed at its first frame with no warning from any build.
- A shader is only correct *in a composition*. `terrain_splat.wgsl` alone is
  meaningless (it uses names from five other files by design), so "validate
  every `.wgsl` in the folder" would flag every real shader as broken and
  would not catch the bug above, which was a mismatch between two files.
- `rae watch` does not know `.wgsl` files exist. Editing one does nothing until
  the app is restarted by hand.
- Run-time disk reads depend on the process cwd. Yesterday's
  `assets/terrain_palette.wgsl` override could not be found by an app whose cwd
  is not its project root (every example), and needed a by-path setter.
- Runtime parameters (the island radius, `islandParamsWgsl()`) are injected as
  generated WGSL text prepended to the file, so the composed text is different
  per run and cannot be checked before that run.

## The design: a declared composition (built 2026-09-19)

A shader becomes a **compile-time value**. The composition is written in Rae,
where the compiler can see it:

```rae
# lib/GrassCompute.rae — inside the function that creates the pipeline (a
# module-level `let` that owns heap is a global, which Rae forbids; the value
# lives on the resource that owns the pipeline)
let grassComputeShader: Shader = shader(
  files: ["lib/noise.wgsl", "lib/world_biome.wgsl", "lib/grass_compute.wgsl"]
)
```

`shader(files:)` is a plain function form the compiler recognises (no `@`
sigil — AGENTS.md; `compiler/src/sema.c` `sema_shader_from_call`,
`compiler/src/shader_compose.c`), and its argument must be a literal list of
string literals: a shader is a build-time fact, not a run-time computation
(anything else is a compile error). At `rae build` / `run` / `watch` the
compiler:

1. **Resolves** each path: next to the `.rae` file that declares the shader
   first (an `assets/…` part), then against the project root (a project's
   own `lib/`), then the toolchain stdlib for a `lib/…` path — the order the
   runtime's `rae_ext_rae_gb_read_shader` used, plus the declaring file's
   directory so an example's assets resolve whatever the cwd. A missing file
   is a compile error at the `shader(...)` line naming every path tried, not
   a "shader file not found" print frames later.
2. **Composes** the text in the written order, with a `// --- <path>` marker
   line before each part, so a wgpu or naga message can be mapped back to
   the file it came from.
3. **Validates** the composed text with `naga` (wgpu's own WGSL front end, as a
   CLI). Any parse or validation error is a **compile error** on the
   `shader(...)` line, quoting naga's message with the location translated
   back to the part file and line: `shader validation failed (naga): no
   definition in scope for identifier: raeBiomeSample — at
   assets/lighting.wgsl:4 (part 2 of the composition)`. If `naga` is not
   installed the build warns once (`warning: shader not validated: naga not
   found (cargo install naga-cli); set RAE_SHADER_VALIDATE=off to silence`)
   and continues; `RAE_SHADER_VALIDATE=require` (what the test suite and the
   examples gate set) turns the missing tool into an error, so CI never
   silently skips; `RAE_SHADER_VALIDATE=off` skips validation; `RAE_NAGA`
   points at the executable (otherwise `PATH`, then `~/.cargo/bin/naga`).
4. **Embeds** the composed text in the binary: the call node becomes the
   literal `Shader { text: "<composed>", sources: [<resolved paths>],
   generation: 0 }`, so the text is a C string literal in the emitted program.
   The runtime never reads a `.wgsl` file for a declared shader: no cwd
   dependence, no `RAE_STDLIB` lookup, no `rae_ext_rae_gb_read_shader` — and
   nothing to preload for a WASM build.
5. **Registers** the files as build inputs: `rae watch` rebuilds when a `.wgsl`
   part changes, exactly as it does for a `.rae` file — this is the shader
   hot reload we already have, for free.

Tests: `compiler/tests/cases/872_shader_compose` (three parts, the composed
text), `873_shader_naga_error` (a part referencing a name a missing part
defines: naga's message at the part file and line), `874_shader_missing_part`,
`875_shader_not_literal`.

### The `Shader` type

A value, not a string — this is what keeps hot reload possible later:

```rae
type Shader {
  text: String          # the composed WGSL (what the pipeline is created from)
  sources: List(String) # the resolved part paths, in order (dev builds; empty in release)
  generation: Int       # bumped when `text` is replaced by a reload
}
```

`createComputePipeline(resources:, shader:, entryPoint:)` and the render
pipeline creators take a `view Shader`, not a `String`. Every existing
`ensure…Pipeline` already caches a pipeline and rebuilds it when a generation
counter changes (`terrainBindGen` for the island radius); with `Shader` as the
carrier of `generation`, that pattern becomes the reload path (below).

### Runtime parameters: WGSL `override` constants, not generated text

Text that today is *generated by Rae code and prepended* — the island radius
and falloff (`islandParamsWgsl()`), the palette selection — moves to what WGSL
provides for exactly this:

```wgsl
override RAE_BIOME_ISLAND_RADIUS: f32 = 60.0;
override RAE_BIOME_ISLAND_FALLOFF: f32 = 22.0;
```

set per pipeline at creation time through `WGPUConstantEntry` (the bindings
expose it; the pipeline creators gain an optional `constants:` list). The
composed text is then **fully static** — the same bytes on every run — which
is the precondition for validating it at build time, and for embedding it. A
per-app *file* override (the terrain palette) is not a runtime value at all: an
app composes its own terrain shader with its own palette part in the list.

### What is validated, exactly

Only real, complete shaders: every `shader(...)` declaration in the program's
module closure, composed as written. Nothing scans folders. A `.wgsl` that no
`shader(...)` names is dead as far as the compiler is concerned (and a lint may
say so later).

Validation covers what naga covers: parsing, type checking, binding layout
consistency within the module, entry points. It does not cover what only the
device knows (whether the pipeline's bind group layout matches the buffers the
app binds, feature limits); those stay run-time errors, but they are a much
smaller class, and they are the app's own mistakes rather than a lib file
drifting from its neighbour.

## Hot reload

Two speeds, one declaration.

**Today, with this design: `rae watch`.** The parts are build inputs, so
saving a shader triggers the watcher's rebuild; naga validates; the app
restarts with its state kept (the existing `lib/hot_reload` path). Seconds,
because it is a full rebuild — but correct, and it catches a broken shader
before the app sees it.

**Later: in-process swap (design only, not built yet).** In a dev-profile
build the `Shader` keeps its `sources`. A file watcher inside the app (the
same watcher `rae watch` uses, in-process) notices a part changed,
re-composes from the sources, validates with naga, and if it passes replaces
`shader.text` and bumps `shader.generation`. Every `ensure…Pipeline` already
compares a generation and rebuilds its pipeline when it moves; the swap is
therefore local to the pipeline that owns that shader, sub-second, with no
restart and no app state touched. If validation fails, the old text stays
and the error goes to the log overlay — the app never sees a broken shader.
Release builds drop `sources` and never touch the disk. Nothing in the
build-time design has to change for this: it is an add-on that reuses the
`Shader` value, the generation counters and the same composition code.

Two rules keep that door open, and they apply from the first task:

1. **Never collapse a `Shader` to a `String` at the call site.** A pipeline is
   created from a `view Shader`; the text is read through it.
2. **Keep every runtime input an `override`.** If a value must vary per run,
   it is a pipeline constant, never a string spliced into the composition —
   otherwise a reload cannot reproduce the text the app is running.

## The naga tool

`naga` is the WGSL front end wgpu itself uses, published as a CLI
(`cargo install naga-cli`; the binary is `naga`). It is a **build-time
toolchain dependency** like gcc — a separate executable the compiler runs, not
a library linked into the compiler or the app. `make setup` installs it when
`cargo` is present and says how to get it otherwise; `rae toolchain status`
prints the executable it will run (`naga: /Users/you/.cargo/bin/naga`) or
that it is missing. Validation is `naga --input-kind wgsl <tempfile>` on a
temp copy of the composed text; the exit code and the `error:` /
`file:LINE:COL` lines of its output are the verdict.

Alternatives considered: linking wgpu-native into the compiler to create a
headless device and call `wgpuDeviceCreateShaderModule` (a 30 MB dylib in the
compiler and a GPU at build time — no); Google's `tint` (equivalent, but wgpu
is what actually runs our shaders, so naga's verdict is the one that matters).

## Migration

Thirteen lib modules compose shaders through `gbReadWgsl` (Gbuffer, GbufferSprite,
GbufferTerrain, GbufferUnderwater, Gpu2dCanvas / -Image / -Text, GrassCompute,
NoiseWgsl, TransparentForward, water/WaterFft, water/WaterReadback,
water/WaterSystem) and five runtime C files carry or read shader text
(runtime_gpu2d_box / _image / _text, runtime_gpu3d_gbuffer, and the deferred
lighting shader as a C string in runtime_gpu3d_deferred.c). The migration
moves each to a `shader(...)` declaration; the C-string shaders move into
`.wgsl` files under `lib/` so they are validated like the rest. The island
params become `override` constants; the terrain palette becomes a part the app
lists. `rae_ext_rae_gb_read_shader` and `gbReadWgsl` are deleted at the end —
the runtime reads no shader from disk.

The sibling game projects compose the same lib shaders; they migrate by
replacing their `gbReadWgsl` calls the same way, and from then on a lib file
that drifts from its neighbour fails *their* build instead of their first
frame.

## Queue

The compile-time construct, validation, embedding, watch inputs and naga in
the toolchain setup are built (2026-09-19). Still queued: the `override`
constants in the pipeline creators and the lib/renderer migration (the
pipeline creators still take a `String`; a lib module composes with
`gbReadWgsl` until it migrates). The in-process swap is documented above and
not queued.
