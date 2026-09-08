# Shared FFT surface cache

`WaterSystem.surfaceCache` owns the CPU side of the realistic water readback.
`waterFftStep` advances it after the FFT work, including frames on which no
cascade updates. The existing Gerstner `waterHeightAt` query is unchanged.

```rae
open water/FftSurfaceCache

# Caller chooses tolerated age and handles unavailability explicitly.
if let sample: FftSurfaceSample = sampleFftSurface(
  cache: waterSystem.surfaceCache, body: body,
  x: position.x, y: position.y, now: waterSystem.time, maxAge: 0.5
) {
  position.z = sample.height
}
```

The result is `none` until every active cascade has current-generation data,
or when any sample exceeds `maxAge`. Incompatible sea-state/tier settings,
non-realistic bodies, negative maximum age and samples ahead of `now` are also
unavailable. Completed samples are never extrapolated; there is no Gerstner
fallback. The caller can retain its previous transform, hide an effect or apply
its own policy when the optional result is absent.

`FftSurfaceSample` contains world height, summed displacement, per-cascade
`sampleTimes`, `tileSizes`, `signedJacobians`, `cascadeCount`, `generation` and
`inversionError`. The Vec3 components identify cascades 0/1/2; inactive components
are zero except Jacobians, which default to one. Jacobians remain separate:
summing their determinants would not give the determinant of the combined field.
Samples retain the time each cascade was actually evolved, rather than the time
its readback completed. Thus the modulo N=2/N=3 schedule produces different ages
in a single result. The oldest active cascade determines availability.

Queries use world x/y coordinates with repeat wrapping and bilinear interpolation
at texel centres, matching the surface sampler convention. Sixteen damped
fixed-point iterations solve `worldXY = gridXY + summedDisplacementXY(gridXY)`.
Height is the body's surface elevation plus displacement z at that recovered
coordinate. Folded surfaces may have multiple solutions; `inversionError` reports
the final absolute x/y residual in metres so a caller can choose its own policy.
The result still represents the latest completed samples. Queries allocate no
collections and perform no GPU work or polling.

## Storage and lifecycle

Each active cascade has one 32×32 float4 snapshot, one 16 KiB storage buffer,
one 16 KiB MapRead staging buffer and at most one pending request. CPU capacity
is bounded at three 16 KiB grids. The WaterSystem lifetime manages GPU handles
through the existing water handle store, slots 137–150, with no new renderer C
entry points. Shutdown/rebuild walks capacity, not a newly changed cascade count.

A small compute pass samples the existing rgba16float displacement map into the
float4 storage buffer. This uses its existing TextureBinding usage, so it needs
neither texture CopySrc usage nor CPU half-float unpacking. It preserves xyz
metres and the signed Jacobian in w, rather than the clamped foam channel. This
is a bilinearly resampled low-resolution approximation, not full-resolution FFT
reconstruction; short waves and tiny folding regions can be missed or aliased.
Gameplay consumers should choose tolerances appropriate to the grid spacing.

Storage-to-staging copies are submitted after the corresponding FFT commands.
Requests carry captured simulation time, tile size and generation; a busy slot
keeps the last completed CPU snapshot and skips launching another read. Once
available it catches up to the latest evolved map. Unchanged update counters
avoid repeated reads of an idle cascade. Normal polling uses the nonblocking
`webgpu/Readback` API. No pointer into a growing List is held while a map waits.

Rebuild, sea-state invalidation, diagnostic spectrum replacement and shutdown
cancel requests, release bindings/buffers and advance the cache generation before
old data can be queried. The platform request keeps its own callback reference
until deferred cancellation is delivered. Old callbacks cannot copy into the
new cache. CPU grids retain bounded capacity until their WaterSystem is dropped.

## Verification

The pure Compiled regression `758_fft_surface_cache` checks missing/stale and
mixed-generation data, timestamp boundaries, three-cascade sums, signed
Jacobians, horizontal inversion, a known cosine wave, negative/positive wrapping
and incompatible sea states. Run through `compiler/tools/watch-tests.sh` with
`TEST=758_fft_surface_cache` and an explicit 120-second timeout.

Example 119's `RAE_WATER_TIER_CHECK=1` hardware check covers all nine size/count
combinations plus shrink/expand, sea-state changes, N=2/N=3 and explicit
invalidation. It compares actual cache timestamps against evolved timestamps,
checks known-wave amplitude/spatial and temporal phase, tests negative Jacobian
readback, cancels pending maps before polling, and checks every handle is clear
after shutdown. Diagnostic polling has a two-second deadline; it is separate
from ordinary per-frame nonblocking polling.

`RAE_WATER_BUOYANCY=1` adds an orange box driven by the optional cache query.
It retains its last transform on `none`. On Apple M1 Max / Metal, a 3.5-second
N=3 run used 108 completed samples and heights from -0.19 to 1.49 m; its rendered
placement at the water surface was inspected. This is a height-following demo,
not a force-based rigid-body simulation.

The 114/118 toon gates remain the renderer regression checks. Shared readback
belongs to #858, SSR to #854, and future underwater/spray consumers to #855/#859.
The agreed update cadence and real elapsed wave clock are unchanged. Existing
blocking readback diagnostics remain available.
