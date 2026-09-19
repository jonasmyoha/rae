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

## Migration (done 2026-09-19)

Every shader the deferred renderer, the 2D canvas, the water and the grass use
is now a declared composition, and the runtime opens no `.wgsl` file:

- **Pipeline creators take a `view Shader`.** `createComputePipeline`,
  `createRenderPipeline`, `createDepthPipeline` (lib/gpu), `createGbufferPipeline`
  and `createFullscreenPipeline` (lib/Gbuffer*) take `shader: view Shader`;
  `createComputePipelineWithConstants` / `createRenderPipelineWithConstants`
  add `constants: view List(ShaderConstant)` — the `override` values, passed
  through as `WGPUConstantEntry` on the compute / vertex / fragment stage.
  The text primitives survive as `create*PipelineSource(wgsl: view String, …)`
  for a program that genuinely builds WGSL at run time (the `zz_gpu_*`
  self-tests); `Gpu.kernelShader(shader:, entry:)` is the compute wrapper's
  form. `ShaderConstant { name, value }` and `noShaderConstants()` are core.
- **Runtime parameters are `override`s.** `world_biome.wgsl` declares
  `override RAE_BIOME_ISLAND_RADIUS: f32 = 60.0;` / `…FALLOFF = 22.0;`;
  `WorldBiome.islandConstants()` hands the values to the terrain and grass
  pipelines at creation (`islandParamsWgsl` is gone), so their composed text
  is static and validated.
- **An app's palette is a part it lists.** `rendererSetTerrainShader(renderer:,
  shader:)` / `terrainSetShader` take the app's own six-part terrain
  composition (its `assets/terrain_palette.wgsl` + `assets/terrain_grass.wgsl`
  in place of the lib parts — example 114); the by-path setter
  `rendererSetTerrainPalette` and the cwd-relative `assets/` probe are gone.
  The grass pass's `surfaceShader` text became `grassSetComputeShader(grass:,
  shader:)`, a declared composition ending in `lib/grass_compute.wgsl`; it
  keeps the island `override` pair, so a surface over `lib/world_biome.wgsl`
  is unchanged. A surface that is NOT the island biome (its own overrides —
  a venue's dimensions, say) uses `grassSetComputeShaderWithConstants(grass:,
  shader:, constants:)`: wgpu rejects an `override` entry the shader does not
  declare, so an app's constants must be exactly its own.
- **The lib compositions** (GbufferSprite, GbufferTerrain, GbufferUnderwater,
  Gpu2dCanvas / -Image / -Text, GrassCompute, TransparentForward,
  water/WaterFft, water/WaterReadback, water/WaterSystem) are `shader(files:)`
  declarations inside the function that creates the pipeline; `NoiseWgsl.rae`
  is gone (examples 107/108 declare `["lib/noise.wgsl", "raymarch.wgsl"]`).
- **The C-string shaders are files.** The thirteen WGSL strings the runtime
  kept (`rae_gb_*_wgsl`, `rae_sm_wgsl_*`) were extracted through the real C
  preprocessor into `lib/gbuffer_static.wgsl`, `gbuffer_skinned.wgsl`,
  `gbuffer_inspect.wgsl`, `gbuffer_sdf.wgsl`, `deferred_light.wgsl`,
  `deferred_ao.wgsl`, `deferred_taa.wgsl`, `deferred_composite.wgsl`,
  `deferred_pyramid_from_depth.wgsl`, `deferred_pyramid_reduce.wgsl`,
  `shadow_static.wgsl`, `shadow_skinned.wgsl`, `shadow_sdf.wgsl`, with the
  shared blocks factored into parts — `lib/gbuffer_octahedral.wgsl` (the normal
  encode, composed before every G-buffer reader/writer) and
  `lib/sky_hosek.wgsl` (composed before `deferred_light.wgsl`). The accessors,
  their stubs and `runtime_gpu3d_gbuffer_sdf.c` are deleted.
- **What is left in C:** the LEGACY forward renderer (`runtime_gpu3d.c` and
  its skin / sdf / sky / ssao files) still creates its own pipelines in C from
  its own strings (`G3D_SHADOW_FN_WGSL`, `runtime_sky_wgsl.h`); it is not the
  supported renderer and is untouched. `rae_ext_rae_gb_read_shader` became
  `rae_ext_rae_read_asset`, the stdlib asset reader for `.raescene` files and
  the sky dataset — no shader goes through it. The WASM preload list no
  longer carries `lib/noise.wgsl`.

The sibling game projects compose the same lib shaders; they migrate by
declaring their compositions the same way (their terrain palette through
`rendererSetTerrainShader`, their grass surface through
`grassSetComputeShader`), and from then on a lib file that drifts from its
neighbour fails *their* build instead of their first frame.

## Queue

Everything above is built (2026-09-19). The in-process swap is documented
above and not queued.
