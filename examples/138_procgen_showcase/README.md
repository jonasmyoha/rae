# Autumn Grove

A procedural prop showcase with six trees, a stone outcrop, low shrubs,
clumped grass and a winding path on a raised garden base.

- **1** restores the complete grove.
- **2** frames the branching wood and leaf crowns.
- **3** frames the stone outcrop.
- Drag to orbit, scroll to zoom, or press **Tab** for the free WASD camera.
- The shared debug widget shows frame rate and memory.

Run from the repository root:

```sh
compiler/bin/rae run --project examples/138_procgen_showcase examples/138_procgen_showcase/Main.rae
```

`RAE_GROVE_VIEW=trees` or `rocks` selects a reproducible initial close-up.
The default is the complete grove. The example gate renders all three views.

## Geometry and shading

`GroveGeometry.rae` builds seeded mesh data in Rae. Trees have tapered,
bending trunks, roots and eleven rising branches. Each branch carries a crown
of small, two-sided six-sided leaves. Leaf normals follow the crown envelope
so light describes its volume without treating every leaf as an unrelated
flat surface. Three material batches provide restrained variation per crown.
The scene has 90,000 leaves; they are generated once, not rebuilt per frame.

The stones use three offset eight-sided rings with broad faces and flatter
caps. Their placement builds one outcrop with a skirt of fallen fragments.
`GroveScene.rae` owns the gold, copper, scarlet and green palette, the grass,
path, ground and prop placement. All geometry is uploaded once and attached
to entities in `World3d`; the deferred renderer owns the uploaded resources.

Every material uses the existing cel-shading flag and the same directional
light and cascaded shadows. The noisy material-swatches overlay and coarse
canopy blobs have been removed from this example. It uses no bitmap textures
or imported prop meshes. The shared procedural generators and shared shading
defaults are unchanged; this is an independently art-directed example.

![The complete grove](references/Grove.png)

![Tree close-up](references/Trees.png)

![Stone close-up](references/Rocks.png)
