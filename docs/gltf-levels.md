# glTF levels

A level authored in Blender (or any glTF exporter) and loaded by
`lib/app3d/GltfLevel` (`readGltfLevel`). The loader gives the level's items in
the file's order; the app makes its entities from them: a drawn mesh, a static
collider (`addLevelCollider`), a named point (`LevelMarker`), each with a
`LevelNode` saying where it came from. What ground a drawn triangle is
(`lib/app3d/SurfaceMap`) and where grass grows (`surfaceGrassField`) follow from
the materials, by rules the app gives.

## Authoring

- Meters, right-handed, Z up, +X right and +Y forward. Apply transforms before
  exporting, and turn **+Y Up off** (`export_yup=False`) when exporting glTF: the
  loader adds no axis rotation.
- The file's origin is where the app places the level (`placement`); a level
  meant to stand on a floor has its origin on it.
- Only the default scene is read. Parent transforms compose (a node's `matrix`,
  or its `translation` / `rotation` / `scale`); each primitive's vertices are baked
  into the world once, normals by the inverse transpose, so what is drawn and what
  collides agree, nested or non-uniformly scaled.
- Opaque base colors: every primitive is drawn in its material's
  `baseColorFactor` RGB (white without one). Textures are not read.
- A mesh node named with the suffix `_collision` is a static triangle-mesh
  collider and is never drawn. Use closed, outward-facing triangles and no zero
  scale. Decorative geometry does not collide unless it has its own collision mesh.
- An empty node named `marker_<name>` is a named point. The name after `marker_`
  must be nonempty and unique in the file; a mesh node with that prefix is a mesh.
- Name materials by what they are (`Grass light`, `Asphalt`, `Rock`): an app's
  surface rules (`MaterialSurface`: a name prefix and its surface) classify the
  ground by them.

## Loading

A bad file, an unreadable mesh, a duplicate marker or a cyclic scene is an error
(`LevelRead.ok` false, with the reason), so an app can stop with a diagnostic
rather than run with a missing wall or an ambiguous spawn point. Box3D owns the
colliders until physics shutdown; the level is static.

## The camera and other categories

Give the shapes that should stop a character camera the camera-blocking category
(`app3d/CameraObstruction.cameraBlockingCategory`): walls and large architecture,
not every prop, or the camera jumps in front of anything that passes. Bit 2 is
reserved by Rae's mover queries.

## The surface map

`createCodedSurfaceMap` lays a grid over the level (cells a couple of meters
square); `layMesh` puts each drawn mesh's triangles into it as the surface its
material stands for (`surfaceOfMaterialName`); every cell keeps the highest
triangle over its center (a road over the terrain under it) and that height.
Surfaces are the app's own codes; `surfaceCodeAt` and `surfaceHeightAt` read
the surface and the height at a point (a footprint lies on the drawn path,
not on the grass collider under it). `surfaceGrassField` is one value per cell for
GPU grass (`GrassCompute.grassSetSurfaceField`): the height where the cell is the
grass surface, none elsewhere.

## A test level

`python3 tools/levels/exportTestLevel.py <out.glb>` writes a small level without
Blender: a 10 m grass plate and its collider, a 2 m stone cube under a lifted
parent and its collider, and `marker_test` authored as a matrix. Fixture
`1208_gltf_level` reads it.

## A level to walk round

`python3 tools/levels/exportWalkLevel.py <out.glb>` writes a richer one: a 40 m
grass field with a sand path up its middle to a wall, two low side walls, a
stone ramp up to a platform, a stack of crates, every solid piece with its
`_collision` twin (the path is paint, drawn only), `marker_spawn` and
`marker_goal`. Example `144_gltf_level` loads it: the meshes drawn in their
base colors as World3d instances, the colliders, a Box3D mover spawned at
`marker_spawn`, and footprints shaded by the surface map. Its
`RAE_LEVEL_SELFTEST=1` walks the character into the wall and up the ramp
headless, and the example gate checks it.
