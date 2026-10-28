"""Write a tiny Z-up glTF level, without Blender or third-party packages.

The authoring rules of app3d/GltfLevel (docs/gltf-levels.md): meters, Z up,
transforms applied. A 10 m grass plate (material "Grass") with its
`ground_collision`, a 2 m stone cube (material "Stone", bottom at zero) under
a parent lifted 1 m, its `cube_collision`, and `marker_test` three meters
above the cube's center, authored as a matrix. Usage:

    python3 tools/levels/exportTestLevel.py <out.glb>
"""
import json
import struct
import sys
from pathlib import Path


def box(half, center):
    positions, normals, indices = [], [], []
    hx, hy, hz = half
    cx, cy, cz = center
    faces = [
        ((1, 0, 0), [(1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)]),
        ((-1, 0, 0), [(-1, 1, -1), (-1, -1, -1), (-1, -1, 1), (-1, 1, 1)]),
        ((0, 1, 0), [(1, 1, -1), (-1, 1, -1), (-1, 1, 1), (1, 1, 1)]),
        ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),
        ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
        ((0, 0, -1), [(-1, 1, -1), (1, 1, -1), (1, -1, -1), (-1, -1, -1)]),
    ]
    for normal, corners in faces:
        base = len(positions) // 3
        for x, y, z in corners:
            positions.extend((cx + x * hx, cy + y * hy, cz + z * hz))
            normals.extend(normal)
        indices.extend(base + index for index in (0, 1, 2, 0, 2, 3))
    return positions, normals, indices


def writeLevelGlb(destination, meshes, materials, nodes, rootNodes, generator):
    """Write a .glb: `meshes` are (positions, normals, indices, material index)
    in world meters, `materials` (name, rgb), `nodes` glTF node dicts, the
    scene's roots `rootNodes`."""
    binary = b''
    views, accessors, gltfMeshes = [], [], []
    for positions, normals, indices, material in meshes:
        count = len(positions) // 3
        for data, kind in ((positions, 'f'), (normals, 'f')):
            chunk = struct.pack(f'<{len(data)}{kind}', *data)
            views.append({'buffer': 0, 'byteOffset': len(binary), 'byteLength': len(chunk)})
            accessor = {'bufferView': len(views) - 1, 'componentType': 5126, 'count': count, 'type': 'VEC3'}
            if data is positions:
                accessor['min'] = [min(data[i::3]) for i in range(3)]
                accessor['max'] = [max(data[i::3]) for i in range(3)]
            accessors.append(accessor)
            binary += chunk
        chunk = struct.pack(f'<{len(indices)}H', *indices)
        chunk += b'\0' * (-len(chunk) % 4)
        views.append({'buffer': 0, 'byteOffset': len(binary), 'byteLength': len(indices) * 2})
        accessors.append({'bufferView': len(views) - 1, 'componentType': 5123, 'count': len(indices), 'type': 'SCALAR'})
        binary += chunk
        base = len(accessors) - 3
        gltfMeshes.append({'primitives': [{'attributes': {'POSITION': base, 'NORMAL': base + 1},
                                           'indices': base + 2, 'material': material}]})
    document = {
        'asset': {'version': '2.0', 'generator': generator},
        'scene': 0,
        'scenes': [{'nodes': rootNodes}],
        'nodes': nodes,
        'meshes': gltfMeshes,
        'materials': [{'name': name, 'pbrMetallicRoughness': {'baseColorFactor': [*rgb, 1]}}
                      for name, rgb in materials],
        'buffers': [{'byteLength': len(binary)}],
        'bufferViews': views,
        'accessors': accessors,
    }
    encoded = json.dumps(document, separators=(',', ':')).encode()
    encoded += b' ' * (-len(encoded) % 4)
    output = (struct.pack('<III', 0x46546C67, 2, 28 + len(encoded) + len(binary))
              + struct.pack('<I4s', len(encoded), b'JSON') + encoded
              + struct.pack('<I4s', len(binary), b'BIN\0') + binary)
    Path(destination).parent.mkdir(parents=True, exist_ok=True)
    Path(destination).write_bytes(output)
    print(f'{destination}: {len(output)} bytes')


def exportLevel(destination):
    ground = box((5, 5, 0.05), (0, 0, -0.05))
    cube = box((1, 1, 1), (0, 0, 0))
    writeLevelGlb(
        destination,
        meshes=[(*ground, 0), (*cube, 1)],
        materials=[('Grass', [0.3, 0.6, 0.25]), ('Stone', [0.6, 0.6, 0.62])],
        nodes=[
            {'name': 'ground', 'mesh': 0},
            {'name': 'ground_collision', 'mesh': 0},
            {'name': 'block', 'translation': [0, 0, 1], 'children': [3, 4, 5]},
            {'name': 'cube', 'mesh': 1},
            {'name': 'cube_collision', 'mesh': 1},
            {'name': 'marker_test', 'matrix': [1, 0, 0, 0, 0, 1, 0, 0,
                                              0, 0, 1, 0, 0, 0, 3, 1]},
        ],
        rootNodes=[0, 1, 2],
        generator='exportTestLevel.py (Z-up meters)')


if __name__ == '__main__':
    exportLevel(sys.argv[1])
