# Reference QR grids from segno for the qr package's fixture: one block per case,
# "case <name> <version> <level> <mask>" then the rows ('#' dark).
import segno, sys
import segno.encoder as encoder
from segno import consts
# segno 1.6.6 adds a whole zero byte of "padding bits" when the bit stream
# already ends on a codeword boundary (8 - length % 8 = 8); ISO/IEC
# 18004:2015 7.4.10 adds padding bits only when it does not. Corrected here.
def write_padding_bits(buff, version, length):
    if version not in (consts.VERSION_M1, consts.VERSION_M3):
        buff.extend([0] * ((8 - (length % 8)) % 8))
encoder.write_padding_bits = write_padding_bits
url100 = "https://example.com/" + "a" * 60 + "?q=" + "b" * 17
assert len(url100) == 100
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
    code = segno.make(text.encode("ascii") if mode == "byte" else text, error=level,
                      mode=mode, version=version, mask=mask, boost_error=False, micro=False)
    print(f"case {name} {code.version} {code.error} {code.mask}")
    for row in code.matrix_iter(border=0):
        print("".join("#" if bit else "." for bit in row))
