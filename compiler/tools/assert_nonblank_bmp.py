#!/usr/bin/env python3
"""Reject missing, malformed, or visually blank BMP screenshots."""

from collections import Counter
from pathlib import Path
import struct
import sys


def fail(message: str) -> None:
    print(f"screenshot validation failed: {message}", file=sys.stderr)
    raise SystemExit(1)


if len(sys.argv) not in (2, 3):
    fail("usage: assert_nonblank_bmp.py <screenshot.bmp> [--gpu3d-ui|--min-colors=N]")

gpu3d_ui = len(sys.argv) == 3 and sys.argv[2] == "--gpu3d-ui"

# The default colour floor (32) assumes a SHADED image, where lighting
# spreads every surface across many values. A raw G-buffer channel is not
# shaded: flat albedo over N materials has close to N colours by
# construction, and asserting 32+ on it would be asserting that the
# renderer is doing something the geometry pass exists not to do. Callers
# that know the expected variety state it instead — the deferred example
# checks its albedo channel against the material count it authored.
min_colors = 32
if len(sys.argv) == 3 and sys.argv[2].startswith("--min-colors="):
    try:
        min_colors = int(sys.argv[2].split("=", 1)[1])
    except ValueError:
        fail(f"--min-colors needs an integer, got: {sys.argv[2]}")
elif len(sys.argv) == 3 and not gpu3d_ui:
    fail(f"unknown validation mode: {sys.argv[2]}")

path = Path(sys.argv[1])
if not path.is_file():
    fail(f"missing file: {path}")

data = path.read_bytes()
if len(data) < 54 or data[:2] != b"BM":
    fail("not a BMP file")

pixel_offset = struct.unpack_from("<I", data, 10)[0]
width, signed_height = struct.unpack_from("<ii", data, 18)
bits_per_pixel = struct.unpack_from("<H", data, 28)[0]
compression = struct.unpack_from("<I", data, 30)[0]
height = abs(signed_height)

if width < 64 or height < 64:
    fail(f"unexpected dimensions: {width}x{height}")
if bits_per_pixel not in (24, 32):
    fail(f"unsupported pixel format: {bits_per_pixel} bpp")
if compression not in (0, 3):
    fail(f"unsupported BMP compression: {compression}")

bytes_per_pixel = bits_per_pixel // 8
row_stride = ((width * bits_per_pixel + 31) // 32) * 4
required_size = pixel_offset + row_stride * height
if required_size > len(data):
    fail(f"truncated pixel data: need {required_size} bytes, got {len(data)}")


def pixel(x: int, y: int) -> tuple[int, int, int]:
    offset = pixel_offset + y * row_stride + x * bytes_per_pixel
    return tuple(data[offset : offset + 3])


def rgb_pixel(x: int, storage_y: int) -> tuple[int, int, int]:
    """Return RGB for a BMP storage-row coordinate."""
    blue, green, red = pixel(x, storage_y)
    return red, green, blue


# The renderer clears to one background color. Derive it from a sparse border
# sample so scene geometry in one corner cannot become the baseline.
border = []
step_x = max(1, width // 128)
step_y = max(1, height // 128)
for x in range(0, width, step_x):
    border.append(pixel(x, 0))
    border.append(pixel(x, height - 1))
for y in range(0, height, step_y):
    border.append(pixel(0, y))
    border.append(pixel(width - 1, y))
background = Counter(border).most_common(1)[0][0]

# Sample at most roughly 250k pixels. Driver-specific AA and tiny floating
# differences are irrelevant; the gate only requires substantial geometry.
sample_step = max(1, int((width * height / 250_000) ** 0.5))
sample_count = 0
changed_count = 0
colors = set()
for y in range(0, height, sample_step):
    for x in range(0, width, sample_step):
        color = pixel(x, y)
        sample_count += 1
        if max(abs(color[i] - background[i]) for i in range(3)) >= 12:
            changed_count += 1
        if len(colors) < 4096:
            colors.add(color)

changed_fraction = changed_count / sample_count
if len(colors) < min_colors:
    fail(f"only {len(colors)} sampled colors (need {min_colors})")
if changed_fraction < 0.01:
    fail(f"only {changed_fraction:.2%} of sampled pixels differ from the background")

if gpu3d_ui:
    # Prove that this is the composed 110 frame rather than merely a non-blank
    # 3D target. The top-left HUD contains bright MSDF text over a dark card,
    # and the top-right settings control contains a saturated teal icon plate.
    bright_counts = [0, 0]
    teal_counts = [0, 0]
    # Runtime screenshots have existed with both BMP row orientations. Check
    # the authored top region in both storage directions so this validates UI
    # content rather than depending on the writer's height-sign convention.
    for orientation in (0, 1):
        for y in range(height * 3 // 100, height * 16 // 100, max(1, height // 300)):
            storage_y = y if orientation == 0 else height - 1 - y
            for x in range(width * 2 // 100, width * 28 // 100, max(1, width // 400)):
                red, green, blue = rgb_pixel(x, storage_y)
                # The offscreen screenshot stores linear color values, so
                # visually white glyphs are around 80 rather than 255.
                if min(red, green, blue) > 35 and max(red, green, blue) - min(red, green, blue) < 25:
                    bright_counts[orientation] += 1
        for y in range(height * 3 // 100, height * 15 // 100, max(1, height // 300)):
            storage_y = y if orientation == 0 else height - 1 - y
            for x in range(width * 88 // 100, width * 99 // 100, max(1, width // 400)):
                red, green, blue = rgb_pixel(x, storage_y)
                if green > red + 10 and blue > red + 10:
                    teal_counts[orientation] += 1
    bright_hud = max(bright_counts)
    teal_control = max(teal_counts)
    if bright_hud < 8:
        fail(f"GPU3D UI HUD text missing: only {bright_hud} bright samples")
    if teal_control < 8:
        fail(f"GPU3D UI settings control missing: only {teal_control} teal samples")

print(
    f"non-blank BMP {width}x{height}: {len(colors)}+ colors, "
    f"{changed_fraction:.1%} foreground samples"
)
