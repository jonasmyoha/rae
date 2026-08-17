"""Convert a Unity FBX prop into one transform-baked, Rae Z-up GLB mesh."""

import os
import sys

import bpy
from mathutils import Vector


arguments = sys.argv[sys.argv.index("--") + 1 :]
source, destination = arguments[0], arguments[1]

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=source)

mesh_objects = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
if not mesh_objects:
    raise RuntimeError(f"No mesh objects in {source}")

# The Rae static glTF loader intentionally reads primitive buffers without
# walking node transforms. Bake the FBX hierarchy first so the committed GLB
# has identity nodes and is directly usable by that loader.
bpy.ops.object.select_all(action="DESELECT")
for obj in mesh_objects:
    obj.select_set(True)
bpy.context.view_layer.objects.active = mesh_objects[0]
bpy.ops.object.convert(target="MESH")
if len(mesh_objects) > 1:
    bpy.ops.object.join()
prop = bpy.context.view_layer.objects.active
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

# Place the model on z=0 and centre its footprint. The terrain pool can then
# position and scale every variant with one ordinary Rae Transform3d.
minimum = Vector((float("inf"), float("inf"), float("inf")))
maximum = Vector((float("-inf"), float("-inf"), float("-inf")))
for vertex in prop.data.vertices:
    minimum.x = min(minimum.x, vertex.co.x)
    minimum.y = min(minimum.y, vertex.co.y)
    minimum.z = min(minimum.z, vertex.co.z)
    maximum.x = max(maximum.x, vertex.co.x)
    maximum.y = max(maximum.y, vertex.co.y)
    maximum.z = max(maximum.z, vertex.co.z)

offset = Vector((-(minimum.x + maximum.x) * 0.5,
                 -(minimum.y + maximum.y) * 0.5,
                 -minimum.z))
for vertex in prop.data.vertices:
    vertex.co += offset
prop.data.update()

os.makedirs(os.path.dirname(destination), exist_ok=True)
bpy.ops.export_scene.gltf(
    filepath=destination,
    export_format="GLB",
    export_apply=True,
    export_animations=False,
    export_materials="NONE",
    export_yup=False,
    use_selection=True,
)
print(f"CONVERTED {source} -> {destination}")
