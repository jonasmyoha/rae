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
