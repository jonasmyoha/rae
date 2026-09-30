#!/usr/bin/env python3
"""Fail unless a rectangle of a BMP screenshot is mostly dark.

usage: assert_bmp_region_dark.py SHOT.bmp X0 Y0 X1 Y1 [MIN_FRACTION]

Samples every 4th pixel of [X0,X1) x [Y0,Y1) (top-left origin, screenshot
pixels) and passes when at least MIN_FRACTION (default 0.3) of them have a mean
channel value below 90. Used by gates to prove a dark UI element (a button,
a bar) is actually drawn where it is laid out, not just created.
"""
import struct
import sys


def main():
    if len(sys.argv) < 6:
        print(__doc__)
        return 2
    path = sys.argv[1]
    x0, y0, x1, y1 = (int(v) for v in sys.argv[2:6])
    need = float(sys.argv[6]) if len(sys.argv) > 6 else 0.3
    data = open(path, "rb").read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    row = ((width * bpp // 8 + 3) // 4) * 4
    dark = total = 0
    for y in range(y0, y1, 4):
        for x in range(x0, x1, 4):
            yy = (height - 1 - y) if height > 0 else y
            o = offset + yy * row + x * (bpp // 8)
            if (data[o] + data[o + 1] + data[o + 2]) / 3 < 90:
                dark += 1
            total += 1
    fraction = dark / total if total else 0.0
    ok = fraction >= need
    print(f"{'ok' if ok else 'FAIL'}: {path} region {x0},{y0}-{x1},{y1} dark fraction {fraction:.2f} (need {need})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
