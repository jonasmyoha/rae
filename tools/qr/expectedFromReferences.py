# Assembles 1286_qr_encoder/expected.txt from the references: the ISO 18004
# Annex I codewords, qrcodegen's grids (nayukiGrids.py) and segno's byte
# capacity table (pip install segno). segnoGrids.py gives the same modules at the
# same masks once its padding is corrected.
# expected.txt for compiler/tests/cases/1286_qr_encoder, from the references
import sys
from segno import consts
out = ["iso codewords [16, 32, 12, 86, 97, 128, 236, 17, 236, 17, 236, 17, 236, 17, 236, 17, 165, 36, 212, 193, 237, 54, 199, 135, 44, 85]"]
out += open(sys.argv[1]).read().rstrip("\n").split("\n")          # qrcodegen grids
def count_bits(version):
    return 8 if version < 10 else 16
for version in range(1, 41):
    caps = consts.SYMBOL_CAPACITY[version]
    row = []
    for name, key in (("L", 1), ("M", 0), ("Q", 3), ("H", 2)):
        row.append(f"{name} {(caps[key] - 4 - count_bits(version)) // 8}")
    out.append(f"capacity {version}: " + " ".join(row))
out += [
    "17 bytes at L: version 1",
    "18 bytes at L: version 2",
    "2953 bytes at L: version 40",
    "2954 bytes at L: does not fit",
    "100 bytes forced into version 3 at M: does not fit",
    "the 100-character URL: version 6, 41 x 41 modules",
]
print("\n".join(out))
