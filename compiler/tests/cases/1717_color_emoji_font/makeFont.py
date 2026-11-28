# Makes tinyEmoji.ttf, the smallest CBDT color font case 1717 reads: a cmap (format 12), two
# ligature lookups (one direct, one through an extension lookup), and one-color PNG glyphs.
#   python3 makeFont.py
import struct, zlib

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

def png(r, g, b, size=8):
    rows = b''.join(b'\x00' + bytes([r, g, b, 255]) * size for _ in range(size))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))

# code point -> glyph
cmap_pairs = [(0x1F1EB, 5), (0x1F1EE, 6), (0x1F3FD, 3), (0x1F44D, 2), (0x1F525, 1)]
# glyph -> color (glyph 0 has none)
colors = {1: (255, 0, 0), 2: (0, 255, 0), 3: (128, 128, 128), 4: (0, 0, 255), 5: (255, 255, 255),
          6: (255, 0, 255), 7: (255, 255, 0)}

groups = b''.join(struct.pack('>III', code, code, glyph) for code, glyph in cmap_pairs)
format12 = struct.pack('>HHIII', 12, 0, 16 + len(groups), 0, len(cmap_pairs)) + groups
cmap = struct.pack('>HH', 0, 1) + struct.pack('>HHI', 3, 10, 12) + format12

# CBDT: a header, then each glyph's record (format 17: small metrics, length, PNG)
cbdt = bytearray(struct.pack('>HH', 3, 0))
offsets = []
for glyph in range(1, 8):
    offsets.append(len(cbdt) - 4)
    image = png(*colors[glyph])
    cbdt += struct.pack('>BBbbB', 8, 8, 0, 8, 8) + struct.pack('>I', len(image)) + image
offsets.append(len(cbdt) - 4)

# CBLC: one bitmap size with one index subtable (format 1, image format 17) for glyphs 1..7
index_subtable = struct.pack('>HHI', 1, 17, 4) + b''.join(struct.pack('>I', o) for o in offsets)
array = struct.pack('>HHI', 1, 7, 8) + index_subtable
size_record = (struct.pack('>III', 8 + 48, len(array), 1) + struct.pack('>I', 0) + bytes(24)
               + struct.pack('>HHBBBB', 1, 7, 8, 8, 32, 1))
cblc = struct.pack('>HHI', 3, 0, 1) + size_record + array

# GSUB: lookup 0 a ligature (2 3 -> 4), lookup 1 an extension (type 7) to a ligature (5 6 -> 7)
def ligature_subtable(first, second, joined, coverage_format):
    ligature = struct.pack('>HHH', joined, 2, second)
    ligature_set = struct.pack('>HH', 1, 4) + ligature
    if coverage_format == 1:
        coverage = struct.pack('>HHH', 1, 1, first)
    else:
        coverage = struct.pack('>HHHHH', 2, 1, first, first, 0)
    head = 8
    return (struct.pack('>HHHH', 1, head + len(ligature_set), 1, head) + ligature_set + coverage)

direct = ligature_subtable(2, 3, 4, 1)
extended = ligature_subtable(5, 6, 7, 2)
extension = struct.pack('>HHI', 1, 4, 8) + extended
lookup0 = struct.pack('>HHHH', 4, 0, 1, 8) + direct
lookup1 = struct.pack('>HHHH', 7, 0, 1, 8) + extension
lookup_list = struct.pack('>HHH', 2, 6, 6 + len(lookup0)) + lookup0 + lookup1
gsub = struct.pack('>HHHHH', 1, 0, 0, 0, 10) + lookup_list

tables = sorted([(b'CBDT', bytes(cbdt)), (b'CBLC', cblc), (b'GSUB', gsub), (b'cmap', cmap)])
header = struct.pack('>IHHHH', 0x00010000, len(tables), 0, 0, 0)
offset = 12 + 16 * len(tables)
directory = b''
body = b''
for tag, data in tables:
    directory += tag + struct.pack('>III', 0, offset + len(body), len(data))
    body += data + bytes((4 - len(data) % 4) % 4)
open('tinyEmoji.ttf', 'wb').write(header + directory + body)
