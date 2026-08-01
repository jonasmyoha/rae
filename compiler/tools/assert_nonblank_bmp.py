#!/usr/bin/env python3
"""Reject missing, malformed, or visually blank BMP screenshots."""

from collections import Counter
from pathlib import Path
import struct
import sys


def fail(message: str) -> None:
    print(f"screenshot validation failed: {message}", file=sys.stderr)
    raise SystemExit(1)


if len(sys.argv) != 2:
    fail("usage: assert_nonblank_bmp.py <screenshot.bmp>")

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
if len(colors) < 32:
    fail(f"only {len(colors)} sampled colors")
if changed_fraction < 0.01:
    fail(f"only {changed_fraction:.2%} of sampled pixels differ from the background")

print(
    f"non-blank BMP {width}x{height}: {len(colors)}+ colors, "
    f"{changed_fraction:.1%} foreground samples"
)
