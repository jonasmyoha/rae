#!/usr/bin/env python3
"""The tree lab's link between the hand-drawn 2D trace of T02 and the
generated tree (docs/reference-tree-plan.md step 3).

  traceTree.py import <trace.js>
      Writes lib/treeGen/TracedT02.rae: every traced limb as a 3D centre line.
      Each dot goes through the lab's near camera onto the tree's upright
      plane (y = 0), so the tree seen from that camera covers the trace. The
      near camera is solved to T02's framing (the trace's base dot on the
      tree's foot, its top dot at the tree's height), so the move and scale
      that `alignment` still applies is almost none.

  traceTree.py overlay <trace.js> <t02.bmp> <render.bmp> <skeleton.txt> <out.bmp>
      Three panels at T02's proportions: T02 with the trace (magenta); the
      render with the trace moved the same way (magenta) and the generated
      tree's real centre lines (cyan, from the lab's RAE_TREELAB_DUMP); and
      the two pictures blended half and half, with both lines on top.

Coordinates: the trace is in crop pixels (330 x 570, y down). The render is
compared through its middle strip in T02's proportions (compare.sh). The
near camera's numbers are labCamera's in Main.rae.
"""
import json
import math
import struct
import subprocess
import sys

HEIGHT = 12.5                     # the tree's height (m): the trace's top
CROP_W, CROP_H = 330.0, 570.0
ASPECT = 1280.0 / 800.0           # the lab's window
FOV_Y = math.radians(50.0)
PITCH = math.radians(18.0)
EYE = (0.69, -15.2, 1.6)
TARGET = (0.69, -15.2 + 10.0 * math.cos(PITCH), 1.6 + 10.0 * math.sin(PITCH))
SAMPLES_TRUNK = 19                # the trunk's points (growTrunk's)
SAMPLES_LIMB = 15                 # every other limb's (growLimb's)
PANEL_W, PANEL_H = 660, 1140


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def scale(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def unit(a): return scale(a, 1.0 / math.sqrt(dot(a, a)))


FORWARD = unit(sub(TARGET, EYE))
RIGHT = unit(cross(FORWARD, (0.0, 0.0, 1.0)))
UP = cross(RIGHT, FORWARD)
TAN = math.tan(FOV_Y / 2.0)

# The picture in "render units": height 1, width ASPECT. The comparison
# strip is the middle (CROP_W / CROP_H) of the height wide.
STRIP_W = CROP_W / CROP_H
STRIP_LEFT = (ASPECT - STRIP_W) / 2.0


def project(point):
    """A world point -> the picture (render units, y down)."""
    d = sub(point, EYE)
    depth = dot(d, FORWARD)
    x = dot(d, RIGHT) / (depth * TAN * ASPECT)
    y = dot(d, UP) / (depth * TAN)
    return ((x + 1.0) / 2.0 * ASPECT, (1.0 - y) / 2.0)


def unproject(picture):
    """A picture point -> where its ray meets the plane y = 0, and the
    metres one render unit spans there."""
    x = picture[0] / ASPECT * 2.0 - 1.0
    y = 1.0 - picture[1] * 2.0
    ray = add(FORWARD, add(scale(RIGHT, x * TAN * ASPECT), scale(UP, y * TAN)))
    t = -EYE[1] / ray[1]
    world = add(EYE, scale(ray, t))
    depth = t * dot(ray, FORWARD)
    return world, 2.0 * depth * TAN


def read_trace(path):
    text = open(path).read()
    start = text.index("\nwindow.TRACE = ") + len("\nwindow.TRACE = ")
    return json.loads(text[start:].rstrip().rstrip(";"))


def crop_to_picture(point):
    """A crop pixel -> the picture where compare.sh shows it."""
    return (STRIP_LEFT + point[0] / CROP_W * STRIP_W, point[1] / CROP_H)


def alignment(trace):
    """The move and scale that puts the trace's base on the tree's foot and
    its top at the tree's height, in the picture."""
    trunk = trace["limbs"][0]["points"]
    base = crop_to_picture(trunk[0])
    top_y = min(p[1] for limb in trace["limbs"] for p in limb["points"])
    top = crop_to_picture((0.0, top_y))
    foot = project((0.0, 0.0, 0.0))
    crown = project((0.0, 0.0, HEIGHT))
    s = (crown[1] - foot[1]) / (top[1] - base[1])

    def place(point):
        p = crop_to_picture(point)
        return (foot[0] + s * (p[0] - base[0]), foot[1] + s * (p[1] - base[1]))
    return place, s


def smooth(points, count):
    """`count` points evenly along the Catmull-Rom curve through `points`
    (the curve trace.html draws), with the radius along it."""
    dense = []
    for i in range(len(points) - 1):
        p0 = points[max(0, i - 1)]
        p1 = points[i]
        p2 = points[i + 1]
        p3 = points[min(len(points) - 1, i + 2)]
        for k in range(40):
            t = k / 40.0
            t2, t3 = t * t, t * t * t
            value = []
            for axis in range(2):
                value.append(0.5 * (2 * p1[axis] + (-p0[axis] + p2[axis]) * t
                                    + (2 * p0[axis] - 5 * p1[axis] + 4 * p2[axis] - p3[axis]) * t2
                                    + (-p0[axis] + 3 * p1[axis] - 3 * p2[axis] + p3[axis]) * t3))
            value.append(p1[2] + (p2[2] - p1[2]) * t)
            dense.append(value)
    dense.append(list(points[-1]))
    lengths = [0.0]
    for a, b in zip(dense, dense[1:]):
        lengths.append(lengths[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    out = []
    j = 0
    for n in range(count):
        want = lengths[-1] * n / (count - 1)
        while j < len(lengths) - 2 and lengths[j + 1] < want:
            j += 1
        span = lengths[j + 1] - lengths[j] or 1.0
        u = (want - lengths[j]) / span
        out.append([dense[j][a] + (dense[j + 1][a] - dense[j][a]) * u for a in range(3)])
    return out


def limb_order(limb):
    if limb["parent"] < 0:
        return 0
    return 1 if "leader" in limb["name"].lower() else 2


def traced_limbs(trace):
    """The traced limbs as world centre lines: [(limb, [(x, y, z, r)])]."""
    place, s = alignment(trace)
    result = []
    for limb in trace["limbs"]:
        order = limb_order(limb)
        count = SAMPLES_TRUNK if order == 0 else SAMPLES_LIMB
        points = []
        for p in smooth(limb["points"], count):
            world, metres = unproject(place(p))
            radius = p[2] / CROP_H * s * metres
            points.append((world[0], 0.0, world[2], radius))
        if order == 0:
            # down to just under the ground (the root flare is in the trace's
            # radii)
            first = points[0]
            points[0] = (first[0], 0.0, min(first[2], 0.0) - 0.25, first[3])
        # a pixel spans more metres higher up (farther from the camera), so
        # two equal traced radii would grow upward: never let one grow
        for i in range(1, len(points)):
            x, y, z, r = points[i]
            points[i] = (x, y, z, min(r, points[i - 1][3]))
        result.append((limb, order, points))
    return result


def write_rae(trace, path):
    lines = [
        "# treeGen/TracedT02: the hand-drawn trace of reference crop T02 as the",
        "# tree's centre lines. GENERATED by examples/146_treeLab/traceTree.py from",
        "# looks-development treeReference/traces/T02.js; do not edit, run",
        "# `examples/146_treeLab/traceTree.py import <trace.js>` again.",
        "# Metres, z up, the foot at the origin, in the upright plane y = 0: seen",
        "# from the tree lab's near camera the tree covers the trace.",
        "import Vec3",
        "open treeGen/TracedLimb",
        "",
        "# The traced tree's height (m)",
        "const tracedT02Height: Float = %.1f" % HEIGHT,
        "",
        "func tracedT02Limbs() ret List(TracedLimb) {",
        "  var limbs: List(TracedLimb) = List.create(TracedLimb, cap: %d)" % len(trace["limbs"]),
    ]
    for limb, order, points in traced_limbs(trace):
        lines.append("  # %s" % limb["name"])
        lines.append("  var %sPoints: List(Vec3) = List.create(Vec3, cap: %d)" % (limb["name"], len(points)))
        lines.append("  var %sRadii: List(Float) = List.create(Float, cap: %d)" % (limb["name"], len(points)))
        for x, y, z, r in points:
            lines.append("  %sPoints.add(value: Vec3.create(x: %.4f, y: %.4f, z: %.4f))" % (limb["name"], x, y, z))
            lines.append("  %sRadii.add(value: %.4f)" % (limb["name"], r))
        lines.append("  limbs.add(value: TracedLimb {")
        lines.append("    points: own %sPoints" % limb["name"])
        lines.append("    radii: own %sRadii" % limb["name"])
        lines.append("    parent: %d" % limb["parent"])
        lines.append("    order: %d" % order)
        lines.append("  })")
    lines.append("  ret limbs")
    lines.append("}")
    open(path, "w").write("\n".join(lines) + "\n")


# --- pictures (24-bit BMP, no libraries) ---

def read_bmp(path):
    data = open(path, "rb").read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    step = struct.unpack_from("<H", data, 28)[0] // 8
    row = (width * step + 3) // 4 * 4
    top_down = height < 0
    height = abs(height)
    rows = []
    for y in range(height):
        source = y if top_down else height - 1 - y
        start = offset + source * row
        line = data[start:start + width * step]
        rows.append([tuple(line[x * step:x * step + 3]) for x in range(width)])
    return width, height, rows


def write_bmp(path, width, height, rows):
    row = (width * 3 + 3) // 4 * 4
    pixels = bytearray()
    for y in range(height - 1, -1, -1):
        line = bytes(v for c in rows[y] for v in c)
        pixels += line + bytes(row - len(line))
    header = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
    open(path, "wb").write(header + info + pixels)


def sample(picture, u, v):
    """The colour at (u, v) in 0..1 of a picture, black outside."""
    width, height, rows = picture
    x, y = int(u * width), int(v * height)
    if 0 <= x < width and 0 <= y < height:
        return rows[y][x]
    return (0, 0, 0)


def panel(source):
    """A PANEL_W x PANEL_H panel; `source(u, v)` gives each pixel's colour."""
    return [[source((x + 0.5) / PANEL_W, (y + 0.5) / PANEL_H) for x in range(PANEL_W)] for y in range(PANEL_H)]


def draw_line(rows, a, b, colour, thickness=2):
    steps = int(max(abs(b[0] - a[0]), abs(b[1] - a[1]))) + 1
    for k in range(steps + 1):
        t = k / steps
        x = a[0] + (b[0] - a[0]) * t
        y = a[1] + (b[1] - a[1]) * t
        for dx in range(-(thickness // 2), thickness - thickness // 2):
            for dy in range(-(thickness // 2), thickness - thickness // 2):
                px, py = int(x) + dx, int(y) + dy
                if 0 <= px < PANEL_W and 0 <= py < PANEL_H:
                    rows[py][px] = colour


def draw_dot(rows, at, colour, size=4):
    for dx in range(-size, size + 1):
        for dy in range(-size, size + 1):
            if dx * dx + dy * dy <= size * size:
                px, py = int(at[0]) + dx, int(at[1]) + dy
                if 0 <= px < PANEL_W and 0 <= py < PANEL_H:
                    rows[py][px] = (32, 0, 32) if dx * dx + dy * dy > (size - 1.5) ** 2 else colour


MAGENTA = (255, 43, 255)   # RGB; written as BGR below
CYAN = (40, 240, 255)


def bgr(colour):
    return (colour[2], colour[1], colour[0])


def read_skeleton(path):
    limbs = []
    for line in open(path):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "limb":
            limbs.append([])
        else:
            limbs[-1].append(tuple(float(v) for v in parts[:3]))
    return limbs


def overlay(trace_path, t02_path, render_path, skeleton_path, out_path):
    trace = read_trace(trace_path)
    t02 = read_bmp(t02_path)
    render = read_bmp(render_path)
    place, _ = alignment(trace)

    def strip_to_panel(picture):
        return ((picture[0] - STRIP_LEFT) / STRIP_W * PANEL_W, picture[1] * PANEL_H)

    def render_at(u, v):
        return sample(render, (STRIP_LEFT + u * STRIP_W) / ASPECT, v)

    # the inverse of `place`, to lay T02 over the render the same way
    ax, ay = place((0.0, 0.0))
    bx, by = place((CROP_W, CROP_H))

    def t02_on_render(u, v):
        px = STRIP_LEFT + u * STRIP_W
        cu = (px - ax) / (bx - ax)
        cv = (v - ay) / (by - ay)
        return sample(t02, cu, cv)

    left = panel(lambda u, v: sample(t02, u, v))
    middle = panel(render_at)
    right = panel(lambda u, v: tuple((a + b) // 2 for a, b in zip(render_at(u, v), t02_on_render(u, v))))
    traced_on_t02 = [[(p[0] / CROP_W * PANEL_W, p[1] / CROP_H * PANEL_H) for p in smooth(limb["points"], 40)]
                     for limb in trace["limbs"]]
    dots_on_t02 = [[(p[0] / CROP_W * PANEL_W, p[1] / CROP_H * PANEL_H) for p in limb["points"]]
                   for limb in trace["limbs"]]
    traced_on_render = [[strip_to_panel(place(p)) for p in smooth(limb["points"], 40)] for limb in trace["limbs"]]
    dots_on_render = [[strip_to_panel(place(p)) for p in limb["points"]] for limb in trace["limbs"]]
    generated = [[strip_to_panel(project(p)) for p in limb] for limb in read_skeleton(skeleton_path)]
    for rows, curves, dots, skeleton in (
        (left, traced_on_t02, dots_on_t02, []),
        (middle, traced_on_render, dots_on_render, generated),
        (right, traced_on_render, dots_on_render, generated),
    ):
        for limb in skeleton:
            for a, b in zip(limb, limb[1:]):
                draw_line(rows, a, b, bgr(CYAN), 3)
        for curve in curves:
            for a, b in zip(curve, curve[1:]):
                draw_line(rows, a, b, bgr(MAGENTA), 2)
        for limb in dots:
            for at in limb:
                draw_dot(rows, at, bgr(MAGENTA))
    gap = 16
    width = PANEL_W * 3 + gap * 2
    rows = [left[y] + [(22, 25, 24)] * gap + middle[y] + [(22, 25, 24)] * gap + right[y] for y in range(PANEL_H)]
    write_bmp(out_path, width, PANEL_H, rows)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "import":
        write_rae(read_trace(sys.argv[2]), "lib/treeGen/TracedT02.rae")
        # the canonical layout, so the tree's format checks pass at once
        subprocess.run(["compiler/bin/rae", "format", "lib/treeGen/TracedT02.rae"], check=True,
                       stdout=subprocess.DEVNULL)
        print("wrote lib/treeGen/TracedT02.rae")
    elif len(sys.argv) == 7 and sys.argv[1] == "overlay":
        overlay(*sys.argv[2:])
    else:
        sys.exit(__doc__)
