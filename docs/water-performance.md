# Water FFT performance

`WaterBody.updateRate = N` means **each cascade updates once every N render
frames**. Cascade `i` runs when `frame % N == i % N`. N=1 updates all active
cascades every frame. With three cascades and N=3, one cascade runs each frame;
with N=2 the work alternates between cascades 0+2 and cascade 1. N greater than
the cascade count leaves some frames with no FFT dispatch.

The app owns wave time. It must advance with real elapsed time even when FFT
updates are skipped. Example 119 supplies absolute elapsed seconds, independent
of the camera's capped movement timestep. An updated cascade evaluates the
spectrum at that current time; a skipped cascade retains its previous map.
There is no interpolation or slowed cascade-local clock.

Initialization, tier changes, sea-state changes and explicit
`waterFftInvalidate` refresh every active map once before the surface can sample
it. This refresh overrides cadence for that frame. The frame ordinal then
continues; changing N alone neither resets it nor re-bakes the spectrum.

## Tiers and ownership

All combinations of FFT size 128/256/512 and cascade count 1/2/3 are supported.
`useMobileWaterTier(body: ...)` in `water/WaterSchedule` selects 128, one cascade,
**N=2**, and refraction off. The desktop constructor remains 256, three cascades,
N=1. Toon water remains the cheaper alternative. Invalid sizes fall back to 256,
counts clamp to 1..3, and N below 1 becomes 1.

The first body owns the shared FFT settings and maps. Surface bindings for
unused cascades borrow cascade 0's valid views; the active-count branch excludes
those extra entries from the sum. A size/count change releases the old surface
bind and FFT resources, allocates the requested tier, and builds fresh bindings.
Cleanup visits the fixed handle capacity, so reducing the requested count before
shutdown does not leave old resources behind. Wave time survives rebuilding.

## Measuring

Example 119 accepts `RAE_WATER_FFT_SIZE`, `RAE_WATER_CASCADES`,
`RAE_WATER_UPDATE_RATE`, and `RAE_WATER_MOBILE=1` for repeatable comparisons.
`RAE_WATER_PERF=1` explicitly enables capture-only `GpuTiming` timestamps around
the evolution/iFFT/post compute pass. Collection blocks for GPU completion;
**normal rendering never collects or waits for timestamps**. Empty schedule
slots report zero GPU work. The measurement excludes spectrum baking, surface
rendering and CPU submission cost.

The schedule is the configured deterministic policy, including on adapters
without timestamp support. No timing-driven adaptive controller is enabled.
A future controller would need nonblocking timing collection before it could
safely change quality during ordinary rendering.

Measured on the development Mac's Apple M1 Max GPU (Metal), 2026-09-08, example 119 at the
same fixed camera and wave time. Each capture ran for five seconds; the first
20 timing samples were discarded. Idle slots count toward the mobile average.

| Capture | Size × cascades | N | Samples | Median GPU ms | Mean GPU ms/frame |
|---|---|---|---|---|---|
| Before scheduling changes | 256 × 3 | all every frame | 404 | 1.418 | 1.485 |
| After, full rate | 256 × 3 | 1 | 148 | 1.755 | 1.950 |
| After, staggered | 256 × 3 | 3 | 147 | 0.623 | 0.686 |
| After, mobile preset | 128 × 1 | 2 | 163 | 0.206 | 0.202 |

These are sequential captures on a shared development machine, not controlled
GPU-clock benchmarks; the full-rate before/after timings show that variability.
Within the after captures, N=3 reduced mean FFT GPU work by about 65%. The
fixed-time N=1 and N=3 screenshots were byte-identical to the original baseline.
The mobile and real-time N=3 captures were also inspected; the mobile tier drops
fine-scale detail and retains coarser swells.

## Regression checks

Case `756_water_schedule` pins the N=1/2/3/5 phase patterns, equal per-cascade
update counts, refresh override, normalization, and the mobile default. Run it
through the official script with an explicit timeout:

```sh
TEST=756_water_schedule perl -e 'alarm shift; exec @ARGV' 180 bash compiler/tools/watch-tests.sh
```

The 119 example gate also runs `RAE_WATER_TIER_CHECK=1`: 14 stages covering all
nine size/count combinations, shrinking and expanding allocations, sea-state
and cadence changes, explicit invalidation, and cleanup after the caller edits
the count before shutdown. It checks actual dispatch counts and recorded sample
times against the modulo rule and current absolute time, draws through the real
surface bindings, and readback-checks the known single-frequency FFT at each
stage. The test starts only when the renderer's GPU target is ready.

The focused example gate takes roughly a minute on this machine; use a bounded
run and keep it exclusive with other test runs:

```sh
MAKEFLAGS='TEST_RUNNER=tools/run_examples.sh' RAE_EXAMPLE_FILTER='114_walker_character 118_water_lake 119_ocean_fft zz_gpu_timing_check' perl -e 'alarm shift; exec @ARGV' 300 bash compiler/tools/watch-tests.sh
```
