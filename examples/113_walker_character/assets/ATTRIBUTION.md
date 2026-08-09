# `walker.glb`

Converted from the Unity **walker** project
(`Assets/characters/eflanir/walker_v010_T-POSE.fbx`) — see
`../tools/README.md` for how to regenerate it.

## Provenance, stated plainly

* **Mesh and textures** — authored for the walker project (artist folder
  `eflanir`). Rights held by the project owner.
* **Skeleton and animation** — **Mixamo**. Every joint carries the
  `mixamorig:` prefix (`mixamorig:Hips`, `mixamorig:Spine`, …) and one
  material is named `deejay`, a Mixamo character. Adobe's Mixamo licence
  permits use of the rig and animations in projects; it is less clear
  about redistributing the rigged source files themselves.

That second point is recorded here deliberately rather than discovered
later. An earlier version of the sibling example shipped a model whose
licensing could not be established, and removing it required rewriting
this repository's history. If the Mixamo terms turn out to preclude
redistribution, this file is the pointer to what needs replacing — and
only the rig would, not the mesh.

## Shape

65 joints, 10 meshes / 12 primitives, ~6700 vertices. Every primitive
carries NORMAL, TEXCOORD_0, JOINTS_0, WEIGHTS_0 and an index buffer; two
also carry vertex colours. The multi-primitive split is what this example
exercises — the Fox in example 112 is a single primitive, so it never
exposed that path.
