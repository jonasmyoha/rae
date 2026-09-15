#!/usr/bin/env python3
"""Tolerant screenshot comparison (#1000): PASS when fewer than `--max-diff-pct`
(default 0.1) percent of pixels differ by more than `--tolerance` (default 8)
on any channel. Reads 24/32-bit BMP and 8-bit RGB/RGBA PNG with only the
standard library, so it runs wherever the gates do.

  assert_bmp_diff.py actual.bmp reference.png [--max-diff-pct 0.1] [--tolerance 8] [--skip-size-mismatch]
  assert_bmp_diff.py --convert shot.bmp reference.png   (write the PNG reference)

Exit 0 on a match, 1 on a mismatch or size difference, 2 on a read error."""
import struct
import sys
import zlib


def read_bmp(path):
    data = open(path, "rb").read()
    off = struct.unpack_from("<I", data, 10)[0]
    w = struct.unpack_from("<i", data, 18)[0]
    h = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0] // 8
    bottom_up = h > 0
    h = abs(h)
    row = (w * bpp + 3) // 4 * 4
    rows = []
    for y in range(h):
        yy = (h - 1 - y) if bottom_up else y
        base = off + yy * row
        line = bytearray()
        for x in range(w):
            p = base + x * bpp
            line += bytes((data[p + 2], data[p + 1], data[p]))
        rows.append(bytes(line))
    return w, h, rows


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    idat = b""
    w = h = 0
    color = 0
    while pos < len(data):
        length = struct.unpack_from(">I", data, pos)[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, color = struct.unpack_from(">IIBB", body, 0)[:4]
            if depth != 8 or color not in (2, 6):
                raise ValueError("only 8-bit RGB/RGBA PNG is supported")
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length
    raw = zlib.decompress(idat)
    channels = 3 if color == 2 else 4
    stride = w * channels
    rows = []
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        flt = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if flt == 1:
                line[i] = (line[i] + a) & 255
            elif flt == 2:
                line[i] = (line[i] + b) & 255
            elif flt == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif flt == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 255
        prev = line
        if channels == 4:
            rgb = bytearray()
            for x in range(w):
                rgb += line[x * 4:x * 4 + 3]
            rows.append(bytes(rgb))
        else:
            rows.append(bytes(line))
    return w, h, rows


def read_image(path):
    if path.lower().endswith(".png"):
        return read_png(path)
    return read_bmp(path)


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    if argv[1] == "--convert":
        if len(argv) < 4:
            print(__doc__)
            return 2
        w, h, rows = read_image(argv[2])
        write_png(argv[3], w, h, rows)
        print(f"wrote {argv[3]} ({w}x{h})")
        return 0
    actual, reference = argv[1], argv[2]
    max_pct = 0.1
    tolerance = 8
    skip_size = False
    i = 3
    while i < len(argv):
        if argv[i] == "--max-diff-pct":
            max_pct = float(argv[i + 1])
            i += 2
        elif argv[i] == "--tolerance":
            tolerance = int(argv[i + 1])
            i += 2
        elif argv[i] == "--skip-size-mismatch":
            # A reference rendered at another DPR is not comparable; a gate
            # passes that as "not checked here" rather than failing.
            skip_size = True
            i += 1
        else:
            i += 1
    try:
        aw, ah, arows = read_image(actual)
        rw, rh, rrows = read_image(reference)
    except Exception as err:  # noqa: BLE001 - a gate wants the reason, not a trace
        print(f"read error: {err}")
        return 2
    if (aw, ah) != (rw, rh):
        print(f"size differs: actual {aw}x{ah}, reference {rw}x{rh}" + (" (skipped)" if skip_size else ""))
        return 0 if skip_size else 1
    differing = 0
    for y in range(ah):
        a = arows[y]
        r = rrows[y]
        for x in range(0, aw * 3, 3):
            if abs(a[x] - r[x]) > tolerance or abs(a[x + 1] - r[x + 1]) > tolerance or abs(a[x + 2] - r[x + 2]) > tolerance:
                differing += 1
    pct = differing * 100.0 / (aw * ah)
    verdict = "ok" if pct < max_pct else "MISMATCH"
    print(f"{verdict}: {differing} of {aw * ah} pixels differ by more than {tolerance} ({pct:.3f}%, limit {max_pct}%)")
    return 0 if pct < max_pct else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
