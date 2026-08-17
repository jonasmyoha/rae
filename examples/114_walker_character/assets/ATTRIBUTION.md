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

## `idle.glb`, `walk.glb`, `run.glb`

Mixamo clips exported for this same rig — animation only, no geometry.
Same licence position as the skeleton above. "Walk fast" in the example
is `walk.glb` played at 1.6x, not a fourth file.

## `environment/*.glb`

Converted from environment FBX files already used by the original Unity
**walker** project. The two trees, rock and flower come from its
`free_lowpoly_forest_nature_pack`; the bush and grass clump come from the
FlatKit demo assets bundled with that project. They are committed as
transform-baked geometry only: Rae supplies its own cel-shaded materials.

The exact source-relative paths and reproducible Blender conversion are in
`../tools/convert_environment.sh`. These remain third-party Unity Asset Store
assets; this attribution records their provenance rather than claiming Rae
authorship.

## `Roboto-Regular.mtsdf.*`

Roboto, Apache 2.0, baked to an MSDF atlas. The same pair of files
examples 103, 104 and 110 ship; duplicated per example rather than
shared, which is this repository's existing convention.

## Shape

65 joints, 10 meshes / 12 primitives, ~6700 vertices. Every primitive
carries NORMAL, TEXCOORD_0, JOINTS_0, WEIGHTS_0 and an index buffer; two
also carry vertex colours. The multi-primitive split is what this example
exercises — the Fox in example 112 is a single primitive, so it never
exposed that path.
