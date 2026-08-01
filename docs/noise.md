# Procedural noise

Rae provides matching procedural-noise families for CPU code and WGSL shaders:

- `lib/noise.rae` is the CPU implementation.
- `lib/noise.wgsl` is the GPU implementation.
- `lib/noise_wgsl.rae` composes the WGSL prelude with an application shader once at pipeline creation.

Both paths use the same 32-bit lattice hash constants, gradient sets, interpolation, octave seeding, and domain-warp offsets. Rae `Float` is now f32 (IEEE-754 binary32, see `primitive-types.md`), so the CPU and GPU sides share the same floating-point **representation** — the width mismatch that previously guaranteed divergence is gone.

That alignment does **not** amount to bit-identical determinism, and this document does not claim it. Results can still differ because operation ordering, contraction (fused multiply-add), the precision of transcendental functions, and compiler optimisation choices are not specified to match between a C compiler and a WGSL shader compiler. Treat CPU and GPU noise as statistically equivalent and spatially stable, and expect agreement to the last few ULP rather than exact equality. Do not build a correctness check on bit-for-bit CPU/GPU comparison.

## Choosing CPU or GPU

Use CPU Rae noise when the result becomes application data:

- generated meshes and collision terrain
- deterministic object or particle spawn data
- cached textures and offline baking
- SSAO sample kernels and tests
- low-frequency simulation or gameplay queries

Use WGSL noise when evaluation is dense or changes every frame:

- per-pixel material variation
- animated SDF displacement and raymarching
- vertex displacement that does not need CPU collision data
- volumetric effects, clouds, and post-processing
- SSAO rotation noise sampled during rendering

Do not generate a full animated noise texture on the CPU and upload it every frame. That adds CPU work and transfer bandwidth while GPUs can evaluate the same function directly. Conversely, do not read GPU noise back merely to build a static mesh; CPU generation is simpler and avoids synchronization.

## CPU API

`hash2`/`hash3` and `value2`/`value3` return values in `[0, 1]`. `perlin2`/`perlin3`, `simplex2`/`simplex3`, and normalized `fbm2`/`fbm3` return approximately `[-1, 1]`. `domainWarp2` and `domainWarp3` return warped coordinates plus the sampled value.

All functions are deterministic, seeded, allocation-free, and safe to call from mesh generation or data-oriented systems. `mesh3d.makeFbmTerrain` is the first library consumer: it samples an FBM height grid once and reuses neighboring heights to derive smooth normals.

## WGSL integration

Shaders that need noise load through:

```rae
import noise_wgsl
open noise_wgsl

let wgsl: String = loadNoiseShader(path: "examples/my_demo/render.wgsl")
```

The prelude is read and concatenated only when the pipeline is created. Rendering then runs compiled WGSL functions such as `raeNoiseSimplex3`, `raeNoiseFbm2`, and `raeNoiseDomainWarp3` without per-frame file or string work.

WGSL has no standard include directive, so host-side composition is the portable sharing mechanism. The `raeNoise` prefix keeps the shared functions from colliding with application shader helpers.
