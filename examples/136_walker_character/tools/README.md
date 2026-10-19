# Regenerating `assets/walker.glb`

The committed `.glb` is derived, not authored. Its source is the Unity
`walker` project's FBX export:

    Assets/characters/eflanir/walker_v010_T-POSE.fbx

Blender does the conversion, because it is the only readily available
tool that handles a skinned FBX's rig and animation faithfully; assimp
also converts, but with more surprises around joint ordering.

    /Applications/Blender.app/Contents/MacOS/Blender -b \
        --python fbx_to_glb.py -- <input.fbx> <output.glb>

`.glb` rather than `.fbx` because FBX is a proprietary format that
`lib/gltf.rae` does not read, and glTF 2.0 is what the renderer targets.
`.glb` rather than `.gltf` because the binary container keeps geometry and
JSON in a single file, so loading needs no relative-path resolution.

The remaining FBX files in that folder are animation clips (Idle, Walking
1-3, Running). They convert the same way and land here when the animation
increment needs them.

## Regenerating the environment props

The infinite meadow uses a curated subset of environment meshes that were
already present in the Unity walker project. Regenerate all committed static
GLBs with:

    ./convert_environment.sh /path/to/walker/Assets

Static props use a separate converter because `lib/gltf.rae` currently reads
primitive buffers without applying glTF node transforms. The converter bakes
the FBX hierarchy, centres each footprint, places its lowest point at z=0 and
exports Rae's Z-up geometry directly. Trees are split into aligned trunk and
crown files so Rae can preserve their two-colour cel-shaded appearance without
depending on the source palette texture.

## Inspecting water splashes

The walker uses `WaterParticles` for small reflective droplet impostors: one
instanced quad mesh with an analytic round silhouette, a clear centre and a
small highlight. Droplets stretch along their projected velocity. No texture
atlas or additional render pass is needed. This is an inexpensive visual
approximation, not physical refraction.

A moving character in shallow water emits eight varied droplets every 0.12 s.
Crowd members alternate across three burst slots. The pool stays at 256 entries;
a stalled frame emits at most one burst. Constant-gravity integration and an
in-place update keep the arc independent of render frequency. Emission still
uses the mean sea plane, as before; it does not follow individual FFT crests.

To inspect a repeatable burst over the actual shore, run from the repo root:

```sh
RAE_WALKER_SPLASH_PREVIEW=1 RAE_WALKER_SPLASH_AGE=0.12 \
RAE_UI_HEADLESS=1 RAE_DEBUG_OVERLAY=0 RAE_SDL_HEADLESS_MS=2400 \
RAE_GPU2D_SCREENSHOT=/tmp/splash.bmp \
perl -e 'alarm shift; exec @ARGV' 180 compiler/bin/rae run \
  --project examples/136_walker_character examples/136_walker_character/Main.rae
```

The preview finds shallow water and positions the camera there. Use ages 0.04,
0.12 and 0.30 seconds to inspect launch, spread and fall. Omit
`RAE_WALKER_SPLASH_AGE` for repeating animated bursts. Neither variable changes
normal play. Compiler fixture `1150_water_particles` checks the arc, billboard
basis, stretch bound and pool expiration without a GPU.
