# QR codes (lib/qr)

`lib/qr` makes QR codes (ISO/IEC 18004:2015) in plain Rae: text or bytes in,
a square grid of modules out.

```rae
open qr/QrTables
open qr/QrEncoder

if let code: QrCode = qrEncodeText(text: invite, level: QrErrorCorrection.medium) {
  # code.size x code.size modules, qrModule(code:, x:, y:) true = dark;
  # draw 4 light modules round it (the quiet zone)
}
```

- `qrEncodeText(text:, level:)` picks the most compact single mode for the
  text: numeric (digits), alphanumeric (`0-9 A-Z space $%*+-./:`) or byte.
- `qrEncodeBytes(bytes:, options:)` is byte mode; `QrOptions` forces a
  version (1-40, 0 = the smallest that fits) or a mask (0-7, -1 = the best).
- Both answer `none` when the data does not fit (2,953 bytes at level L in
  version 40 is the most), or an option is out of range.
- `qrByteCapacity(version:, level:)` is how many bytes a version holds.

The pieces: `QrTables.rae` (blocks and error-correction codewords per
version and level, alignment positions), `ReedSolomon.rae` (GF(256) with the
polynomial 0x11D), `QrEncoder.rae` (the bit stream, block interleaving, the
function patterns, the zigzag placement, the eight masks scored with the
four penalty rules, format and version information). One segment per code:
no mixed modes, ECI, Kanji or structured append.

## Checked against

`compiler/tests/cases/1286_qr_encoder`: the ISO 18004 Annex I example
("01234567", 1-M) gives the standard's codewords; eleven codes (numeric,
alphanumeric, byte at every level, a 100-character URL, forced versions 7,
10, 27 and 40, forced masks) are the same modules as Project Nayuki's
qrcodegen, mask choice included; the byte capacity of all 160 version and
level pairs matches segno's table. `tools/qr/` regenerates the expected
grids.

segno 1.6.6, the other reference, differs in two ways that still scan: it
adds a whole zero byte of padding bits when the bit stream already ends on a
codeword boundary (ISO 7.4.10 adds none), and its penalty score picks
another mask for some codes. With its padding corrected it gives the same
modules at the same mask (`tools/qr/segnoGrids.py`).
