# The reference grids of compiler/tests/cases/1286_qr_encoder, from Project
# Nayuki's qrcodegen (pip install qrcodegen). Usage:
#   python3 tools/qr/nayukiGrids.py > grids.txt
#   python3 tools/qr/expectedFromReferences.py grids.txt > compiler/tests/cases/1286_qr_encoder/expected.txt
# Mask choice and grid from Nayuki's qrcodegen for the same cases
from qrcodegen import QrCode, QrSegment
url100 = "https://example.com/" + "a" * 60 + "?q=" + "b" * 17
E = {"l": QrCode.Ecc.LOW, "m": QrCode.Ecc.MEDIUM, "q": QrCode.Ecc.QUARTILE, "h": QrCode.Ecc.HIGH}
cases = [
    ("iso", "01234567", "numeric", "m", None, None),
    ("hello", "HELLO WORLD", "alphanumeric", "q", None, None),
    ("byteL", "Hello, world!", "byte", "l", None, None),
    ("byteH", "https://example.com", "byte", "h", None, None),
    ("url100", url100, "byte", "m", None, None),
    ("version7", "rae", "byte", "l", 7, None),
    ("version10", "rae relay invite", "byte", "q", 10, None),
    ("version27", "Rae", "byte", "h", 27, None),
    ("version40", "the largest symbol", "byte", "l", 40, None),
    ("mask3", "mask three", "byte", "m", None, 3),
    ("mask6", "mask six", "byte", "h", None, 6),
]
for name, text, mode, level, version, mask in cases:
    if mode == "numeric": seg = QrSegment.make_numeric(text)
    elif mode == "alphanumeric": seg = QrSegment.make_alphanumeric(text)
    else: seg = QrSegment.make_bytes(text.encode("ascii"))
    lo, hi = (version, version) if version else (1, 40)
    code = QrCode.encode_segments([seg], E[level], lo, hi, -1 if mask is None else mask, False)
    print(f"case {name} {code.get_version()} {level.upper()} {code.get_mask()}")
    for y in range(code.get_size()):
        print("".join("#" if code.get_module(x, y) else "." for x in range(code.get_size())))
