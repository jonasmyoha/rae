# Landscape comparison views

This is the fixed visual baseline for Autumn Grove (138) and Walker in a Huge
Forest (143). It exists to compare later tree, rock, grass and flower work from
the same viewpoints. It is not an engine benchmark or a claim that the current
art is finished.

![Nine fixed landscape views](images/landscape-comparisons/baseline.jpg)

The 138 menu's existing app section shows the botanical name and purpose of the
active view. The names match [the species inventory](environment-art-direction.md).
The shared scene chooser has three buttons, while keys 1-9 and
`RAE_GROVE_VIEW` select all nine named views.

## Reproduce the captures

From the repository root:

```sh
tools/capture-landscape-comparisons.sh captures
tools/capture-landscape-comparisons.sh verify
tools/capture-landscape-comparisons.sh metrics
```

The default output is `/tmp/rae-landscape-comparisons`. Set
`RAE_LANDSCAPE_OUTPUT` to use another temporary directory. BMP frames, logs and
repeat captures remain outside git. The script writes `manifest.txt` beside
them, temporarily fixes both windows at 1280 by 800 points, clears saved walker
state for the run, and restores the previous files when it exits. PNGs are
normalized to 1280 by 800 pixels so Retina scale does not change the comparison.

Fixed captures render 60 frames at a fixed 1/60 second step. They therefore
show one simulated second, independent of machine speed. The stability check
captures the camphor-root view twice and the middle forest-walk view twice and
requires each BMP pair to be byte-identical.

## Views and conditions

| Capture | Subject | Existing control | Extra camera state |
|---|---|---|---|
| `138-roots` | Bare camphor wood, roots and foliage underside | `RAE_GROVE_VIEW=roots` | preset |
| `138-trees-side` | Sugar, red and Japanese maples plus date palm | `RAE_GROVE_VIEW=trees` | preset side view |
| `138-foliage-close` | Small-leaved lime crown | `RAE_GROVE_VIEW=fluffy` | preset close view |
| `138-birch-backlit` | Silver birch sprays against the bright sky | `RAE_GROVE_VIEW=birches` | `0 2.5 0.1 0 22 0 0 0 -19 -10 0 0` |
| `138-rocks` | Path-side faceted rock group | `RAE_GROVE_VIEW=rocks` | preset |
| `143-ground-cover` | GPU grass, instanced grass and the orange flower prop | fixed forest start | `1 0 0.24 -0.20 4.2 0 18.0 0.35 0 0 0 0` |
| `143-walk-00` | Forest path start | fixed forest start | `1 0 0.24 -0.06 4.2 0 -18 2.4 0 0 0 0` |
| `143-walk-01` | Forest path middle | fixed forest start | `1 0 0.24 -0.06 4.2 0 -5 2.4 0 0 0 0` |
| `143-walk-02` | Forest path end | fixed forest start | `1 0 0.24 -0.06 4.2 0 8 2.4 0 0 0 0` |

The twelve camera values are the existing `CameraRig.stateString` order: mode,
yaw, orbit pitch, free pitch, distance, free position x/y/z, orbit target x/y,
smooth-mouse flag and smooth-key flag.

Autumn Grove uses Preetham at 10:00, exposure 1.0 and a paused clock. Its fixed
source seeds include the six legacy trees (501, 605, 710, 301, 903 and 217),
fluffy prototypes/placement (73 and 4177), camphor (219), the rock groups listed
in `GroveScene.rae`, and birch's shared prototype (1437). The capture has
63 trees, 21 generated bushes and 7 scarlet shrubs.

The forest uses Preetham on day 298 at 12:00, exposure 1.0, and the current
deterministic placement seeds: density 401, position 811/812, keep 813, yaw 814,
and species 733/977/131. It plants 31,090 trees. Both applications use deferred
rendering, toon materials, cascaded shadows, SSAO, SMAA, bloom, TAA off, GPU
grass and wind. The forest capture also fixes DRS off and runs FFT water.

## Current technical baseline

Measured on 2026-10-25 on a 10-core Apple M1 Max with a 32-core GPU and 64 GB
memory. The hidden windows were 1280 by 800 points; the renderer reported a
2304 by 1440 internal scene for the timed forest run. Compilation was excluded.

| App | G-buffer instances | Resident meshes | Resident indexed triangles | Later frame windows | Settled GPU-pass total | WGPU resources after warm-up |
|---|---:|---:|---:|---|---:|---|
| 138 | 388 | 120 | 1,088,028 | 9.7-10.4 ms; CPU 4.7 ms, GPU wait 4.9-5.6 ms | 11.7-13.2 ms | 369 buffers, 30 textures, 33 views, 72 bind groups, 33 render and 2 compute pipelines |
| 143 | 3,752 | 398 | 814,253 | 18.7-19.5 ms; CPU 5.3-5.8 ms, GPU wait 13.3-14.1 ms | 20.3 ms | 955 buffers, 55 textures, 113 views, 154 then 152 bind groups, 40 render and 11 compute pipelines |

`G-buffer instances` is the renderer's last-frame instance-record count. It is
not a low-level API draw-call count because an instanced batch contributes many
records to one submission. `Resident indexed triangles` sums every indexed mesh
currently stored by the renderer. It excludes procedural GPU grass, water,
fullscreen passes, culled-instance submission and impostor overdraw. These two
numbers are useful for detecting relative changes under the same capture; they
must not be presented as the complete frame workload.

The WGPU reports showed no resource growth after warm-up; 143 released two bind
groups before its final report. GPU totals are the renderer's diagnostic span
sum and include the active render graph, including water and post-processing.
The run used a hidden offscreen window with no input, one app, and ordinary
desktop background load. CPU scheduling, thermal state, driver work and timing
instrumentation affect the result. Use the numbers only as an idle-load
baseline on this machine, not as a portable performance score.
