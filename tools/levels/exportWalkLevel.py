"""Write a small Z-up glTF level to walk round, without Blender.

The authoring rules of app3d/GltfLevel (docs/gltf-levels.md): metres, Z up,
transforms applied, the origin on the floor. A 40 m grass field with a sand
path up its middle to a wall across it, two low side walls, a stone ramp up
to a stone platform, a stack of wooden crates; every solid piece has its
`<name>_collision` twin (the path is paint on the grass, drawn only);
`marker_spawn` at the path's foot and `marker_goal` on the platform. The
materials are named for what they are, so an app's surface rules classify
the ground (Grass, Path, Stone, Wood, Wall). Usage:

    python3 tools/levels/exportWalkLevel.py <out.glb>
"""
import sys
from exportTestLevel import writeLevelGlb


def solid(corners):
    """A closed solid from its eight corners, indexed by bits x (1), y (2),
    z (4): flat-shaded quads, each wound to face out; a face squashed to no
    area (a ramp's thin end) is left out."""
    faces = [(0, 2, 6, 4), (1, 5, 7, 3), (0, 4, 5, 1), (2, 3, 7, 6), (0, 1, 3, 2), (4, 6, 7, 5)]
    middle = [sum(corner[axis] for corner in corners) / 8 for axis in range(3)]
    positions, normals, indices = [], [], []
    for face in faces:
        points = [corners[index] for index in face]
        # Newell's normal of the quad
        normal = [0.0, 0.0, 0.0]
        for i in range(4):
            a, b = points[i], points[(i + 1) % 4]
            normal[0] += (a[1] - b[1]) * (a[2] + b[2])
            normal[1] += (a[2] - b[2]) * (a[0] + b[0])
            normal[2] += (a[0] - b[0]) * (a[1] + b[1])
        length = sum(component * component for component in normal) ** 0.5
        if length < 1e-6:
            continue
        normal = [component / length for component in normal]
        centre = [sum(point[axis] for point in points) / 4 for axis in range(3)]
        if sum(normal[axis] * (centre[axis] - middle[axis]) for axis in range(3)) < 0:
            points.reverse()
            normal = [-component for component in normal]
        base = len(positions) // 3
        for point in points:
            positions.extend(point)
            normals.extend(normal)
        indices.extend(base + index for index in (0, 1, 2, 0, 2, 3))
    return positions, normals, indices


def block(low, high):
    """An axis-aligned box from its lowest to its highest corner."""
    return solid([(high[0] if i & 1 else low[0], high[1] if i & 2 else low[1],
                   high[2] if i & 4 else low[2]) for i in range(8)])


def ramp(low, high):
    """A ramp over the box low..high, rising along +y from nothing to its top."""
    corners = []
    for i in range(8):
        y = high[1] if i & 2 else low[1]
        top = high[2] if i & 2 else low[2]
        corners.append((high[0] if i & 1 else low[0], y, top if i & 4 else low[2]))
    return solid(corners)


MATERIALS = [
    ('Grass', [0.33, 0.58, 0.24]),
    ('Path', [0.78, 0.66, 0.45]),
    ('Stone', [0.58, 0.58, 0.6]),
    ('Wood', [0.62, 0.42, 0.22]),
    ('Wall', [0.9, 0.86, 0.78]),
]

# (name, mesh, material, collides)
PIECES = [
    ('ground', block((-20, -20, -0.2), (20, 20, 0)), 'Grass', True),
    ('path', block((-1.5, -17, 0), (1.5, 8.75, 0.02)), 'Path', False),
    ('wall', block((-6, 8.75, 0), (6, 9.25, 2.5)), 'Wall', True),
    ('wall_west', block((-8.25, -2, 0), (-7.75, 9.25, 1.2)), 'Wall', True),
    ('wall_east', block((7.75, -2, 0), (8.25, 9.25, 1.2)), 'Wall', True),
    ('ramp', ramp((3.5, -4, 0), (6.5, 2, 1.5)), 'Stone', True),
    ('platform', block((3.5, 2, 0), (6.5, 6, 1.5)), 'Stone', True),
    ('crate_a', block((-5.5, -3, 0), (-4.5, -2, 1)), 'Wood', True),
    ('crate_b', block((-4.3, -3.2, 0), (-3.3, -2.2, 1)), 'Wood', True),
    ('crate_c', block((-5.0, -2.8, 1), (-4.0, -1.8, 2)), 'Wood', True),
]

MARKERS = [('marker_spawn', (0, -14, 0)), ('marker_goal', (5, 4, 1.5))]


def exportWalkLevel(destination):
    materialIndex = {name: index for index, (name, _) in enumerate(MATERIALS)}
    meshes, nodes = [], []
    for name, mesh, material, collides in PIECES:
        meshes.append((*mesh, materialIndex[material]))
        nodes.append({'name': name, 'mesh': len(meshes) - 1})
        if collides:
            nodes.append({'name': f'{name}_collision', 'mesh': len(meshes) - 1})
    for name, point in MARKERS:
        nodes.append({'name': name, 'translation': list(point)})
    writeLevelGlb(destination, meshes=meshes, materials=MATERIALS, nodes=nodes,
                  rootNodes=list(range(len(nodes))), generator='exportWalkLevel.py (Z-up metres)')


if __name__ == '__main__':
    exportWalkLevel(sys.argv[1])
