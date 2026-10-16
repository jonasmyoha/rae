# Autumn Grove

A procedural prop showcase with six trees (five with branching wood, one
palm-style), rock groups across the island, low shrubs, the standard GPU grass
and a winding path on a raised garden base.

- **1** restores the complete grove.
- **2** frames the branching wood and leaf crowns.
- **3** frames the stone outcrop.
- Drag to orbit, scroll to zoom. The camera bar's **Orbit** button switches to
  the free WASD camera (Q/E down/up, shift to boost) and back.
- The menu button opens the settings panel: the three views (Scene 1-3), sky
  model, time of day and date, shadows and TAA.
- The chrome is the shared editor panel (`lib/app3d/EditorPanel.rae`); the
  debug widget on top shows frame rate and memory.

Run from the repository root:

```sh
compiler/bin/rae run --project examples/138_procgen_showcase examples/138_procgen_showcase/Main.rae
```

`RAE_GROVE_VIEW=trees` or `rocks` selects a reproducible initial close-up.
The default is the complete grove. The example gate renders all three views.

## Geometry and shading

`GroveGeometry.rae` builds seeded mesh data in Rae. Five trees have ordinary
wood: a straight, slightly leaning trunk with a flared foot, a leader that
carries on up, and three or four main limbs that each fork in two. Every part
is one continuous tapered tube: the segments share their rings, so a limb
bends and thins without a step at any joint, and each limb starts inside its
parent at the parent's thickness. A leaf crown sits on every limb tip. The
green tree keeps the older palm-style stem as its own species.

Each crown is made of small, two-sided six-sided leaves. Leaf normals follow
the crown envelope so light describes its volume without treating every leaf
as an unrelated flat surface. Three material batches give each crown a
restrained colour variation.

The rocks are the lib's faceted rocks (`lib/procgen/Rock.rae`), stretched,
turned and sunk a little into the ground: an outcrop by the path, nine smaller
groups over the island (one big rock with smaller ones around it) and single
pebbles wherever the ground is clear.

The grass is the standard lib GPU grass (`lib/GrassCompute.rae`, the walker's,
with its tuned settings): a compute pass places the blades each frame and
the G-buffer pass draws them. `assets/grove_surface.wgsl` gives that pass the
grove's ground: the island's height, grass inside the rim and none on the
path. The rocks and trunks go to it as obstacle circles in the surface field
(`GroveGrass.rae`), so no blade grows through them.

`GroveScene.rae` owns the gold, copper, scarlet and green palette, the path,
the ground and every placement. All meshes are uploaded once and attached to
entities in `World3d`; the deferred renderer owns the uploaded resources. Every
material uses the existing cel-shading flag and the same directional light
and cascaded shadows. The example uses no bitmap textures or imported meshes.

![The complete grove](references/Grove.png)

![Tree close-up](references/Trees.png)

![Stone close-up](references/Rocks.png)
