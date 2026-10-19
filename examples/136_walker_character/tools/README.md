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

Each character owns left/right foot-contact history in an ECS component. The
rendered toe-bone positions (including animation blending and the physics-driven
root transform) determine contact and velocity. Entering water emits an impact
burst; travelling through water emits weaker directed spray. A held pose and
feet above the surface emit nothing. Launch direction inherits foot motion,
and impact speed controls the number and upward speed of droplets.

The latest completed FFT height samples are used when younger than 0.15 s.
Unavailable/stale data uses the mean sea level; changing between those sources
resets contact history to avoid a false impact. Dry ground skips the FFT query.
First observations, teleports and frames longer than 0.2 s also reset history.
This is a visual contact response, not a fluid solver or a change to physics.
The existing 256-particle cap and allocation-free updates remain.

`RAE_WALKER_SPLASH_TEST=1` runs the actual walking clip through both foot
contacts, checks that a paused pose and airborne points stay quiet, logs
`[splash-pose] ... ok=true`, and exits. Run with an explicit 180 s timeout.
Compiler fixture `1151_water_contact` covers impacts, weaker underwater sweeps,
cooldown, teleports, dry ground and reacquisition. The material preview below
intentionally keeps its synthetic timed bursts, isolated from normal play.

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
