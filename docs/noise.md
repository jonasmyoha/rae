# Procedural noise

Rae provides matching procedural-noise families for CPU code and WGSL shaders:

- `lib/noise.rae` is the CPU implementation.
- `lib/noise.wgsl` is the GPU implementation.
- `lib/noise_wgsl.rae` composes the WGSL prelude with an application shader once at pipeline creation.

Both paths use the same 32-bit lattice hash constants, gradient sets, interpolation, octave seeding, and domain-warp offsets. CPU and GPU samples are statistically equivalent and spatially stable, but are not guaranteed bit-identical because Rae `Float` is wider than WGSL `f32`.

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
