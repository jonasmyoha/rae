"""Convert a Unity FBX tree into aligned Rae Z-up trunk and crown GLBs."""

import os
import sys

import bpy
from mathutils import Vector


arguments = sys.argv[sys.argv.index("--") + 1 :]
source, trunk_destination, crown_destination = arguments[0], arguments[1], arguments[2]

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=source)

mesh_objects = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
if not mesh_objects:
    raise RuntimeError(f"No mesh objects in {source}")

bpy.ops.object.select_all(action="DESELECT")
for obj in mesh_objects:
    obj.select_set(True)
bpy.context.view_layer.objects.active = mesh_objects[0]
bpy.ops.object.convert(target="MESH")
if len(mesh_objects) > 1:
    bpy.ops.object.join()
tree = bpy.context.view_layer.objects.active
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

minimum = Vector((float("inf"), float("inf"), float("inf")))
maximum = Vector((float("-inf"), float("-inf"), float("-inf")))
for vertex in tree.data.vertices:
    minimum.x = min(minimum.x, vertex.co.x)
    minimum.y = min(minimum.y, vertex.co.y)
    minimum.z = min(minimum.z, vertex.co.z)
    maximum.x = max(maximum.x, vertex.co.x)
    maximum.y = max(maximum.y, vertex.co.y)
    maximum.z = max(maximum.z, vertex.co.z)

offset = Vector((-(minimum.x + maximum.x) * 0.5,
                 -(minimum.y + maximum.y) * 0.5,
                 -minimum.z))
for vertex in tree.data.vertices:
    vertex.co += offset
tree.data.update()

# These authored low-poly trees use disconnected meshes for the rooted trunk
# and crown clusters. The trunk is the only component touching z=0.
bpy.context.view_layer.objects.active = tree
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.select_all(action="SELECT")
bpy.ops.mesh.separate(type="LOOSE")
bpy.ops.object.mode_set(mode="OBJECT")
parts = [obj for obj in bpy.context.selected_objects if obj.type == "MESH"]
trunk_parts = []
crown_parts = []
for part in parts:
    lowest = min(vertex.co.z for vertex in part.data.vertices)
    if lowest < 0.001:
        trunk_parts.append(part)
    else:
        crown_parts.append(part)

if not trunk_parts or not crown_parts:
    raise RuntimeError(f"Could not split trunk and crown in {source}")


def join_parts(parts_to_join):
    bpy.ops.object.select_all(action="DESELECT")
    for part in parts_to_join:
        part.select_set(True)
    bpy.context.view_layer.objects.active = parts_to_join[0]
    if len(parts_to_join) > 1:
        bpy.ops.object.join()
    return bpy.context.view_layer.objects.active


trunk = join_parts(trunk_parts)
trunk.name = "trunk"
crown = join_parts(crown_parts)
crown.name = "crown"


def export_part(part, destination):
    bpy.ops.object.select_all(action="DESELECT")
    part.select_set(True)
    bpy.context.view_layer.objects.active = part
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


export_part(trunk, trunk_destination)
export_part(crown, crown_destination)
print(f"CONVERTED {source} -> {trunk_destination}, {crown_destination}")
