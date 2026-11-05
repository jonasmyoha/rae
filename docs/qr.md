# QR codes (the qr package)

`packages/qr` makes QR codes (ISO/IEC 18004:2015) in plain Rae: text or bytes
in, a square grid of modules out. It is a package, so a project declares it:

```
dependencies: {
  dep qr: {
    git: "https://github.com/jonasmyoha/rae.git"
    subdir: "packages/qr"
    version: "1.0.0"
    hash: "<written by rae fetch>"
  }
}
```

or `./rae add qr --git https://github.com/jonasmyoha/rae.git --subdir packages/qr
--version 1.0.0` and `./rae fetch`. Its modules keep the `qr/` prefix.

```rae
open qr/QrTables
open qr/QrEncoder

if let symbol: QrSymbol = qrEncodeText(text: invite, level: QrErrorCorrection.medium) {
  # symbol.size x symbol.size modules, qrModule(symbol:, x:, y:) true = dark;
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

## In 3D: a picture for a material

`qr/QrImage.rae`: `qrImagePixels(symbol:, side:, quietZone:)` makes
`side` x `side` linear RGBA pixels, black modules on white, every module the
same whole number of pixels (crisp: no module is blended into its neighbor)
and centered, the side's remainder going to a wider quiet zone. 512 suits
the deferred renderer's made images:

```rae
let layer: Int = rendererAddSurfaceImagePixels(
  renderer: renderer
  name: "qr-plate"
  pixels: qrImagePixels(symbol: symbol, side: 512, quietZone: 4)
)
material.colorTextureLayer = layer
```

The material's sampler filters linearly, so up close a module's edge is
one texel soft; a version 3 code is 13 texels per module at 512, which scans
from any distance the code is readable at. `makeBox`'s faces all show a
picture the right way round (they used to mirror it on two sides), so a box
carries a code that scans from every side.

## In the UI: the QrCode node

`qr/qrCodeSystem`: a `QrCode` component on a `.raescene` node,

```json
"Invite": {
  "Rect": { "x": 1016, "y": 52, "w": 240, "h": 240 },
  "QrCode": { "text": "https://example.com/join", "errorCorrection": "M", "quietZone": 4 }
}
```

draws the code in the node's laid-out box, as large as the box allows, as
whole-pixel rectangles (runs of dark modules along rows and along columns,
so no antialiased seam shows between two dark modules). The system owns its
tables: the app hands it the scene's pending components
(`qrCodeTakePending(world:, qrCodeSystem:)` after mounting the scene) and
paints after the UI's own render (`qrCodePaint`, same canvas). A code is
encoded again only when its text or level changes; `qrCodeSetText` changes
it from code. On the walker kit the game owns the `QrCodeSystem` and calls
`qrCodePaint` from its `walkerGamePaint` hook (docs/walker-kit.md, example 148).

## Example 147

`examples/147_qr_codes`: a box with a code on every face, turning, and a
`QrCode` node in the UI. `RAE_QR_TEST_FRAME=1` holds the box still;
`RAE_QR_YAW` / `RAE_QR_PITCH` / `RAE_QR_DISTANCE` aim the camera. Checked
on screenshots with OpenCV's QR detector (opencv-python 5.0): the default
frame decodes both codes; straight on, all four sides and the top decode
(before the `makeBox` fix two sides read only mirrored).
`compiler/tests/cases/1287_qr_image` checks the picture module by module.

## Reading: qr/QrReader

```rae
if let result: QrReadResult = qrRead(image: gray) {
  # result.text, result.bytes, result.version, result.level,
  # result.corrected (codewords Reed-Solomon fixed), result.mirrored,
  # result.topLeft / topRight / bottomLeft (the finders in the image)
}
```

`gray` is a `GrayImage` (qr/GrayImage: width, height, one `UInt8` per
pixel, 0 black). The steps, all plain Rae:

1. Binarize with an adaptive threshold (Bradley's: darker than the mean of
   the square round the pixel by 15 %, means from an integral image), so
   uneven light and shadows do not matter.
2. Find the finder patterns: each row's runs in the ratio 1:1:3:1:1,
   confirmed vertically, horizontally and diagonally, merged; the three
   that make the best right isosceles triangle (module sizes up to 3x apart
   and sides up to 3x apart, for steep perspective), ordered by the sign of
   their cross product.
3. The module count from each side in its own module size, then its
   neighbors (+-4, +-8) if that fails; the alignment pattern searched near
   where the three finders put it; a homography from the four points
   (qr/QrPerspective).
4. Each module the majority of five samples; the format information (the
   nearest of the 32 valid codes, up to 3 bits off, either copy); from
   version 7 the version information the same way (and the grid sampled
   again if it names another size).
5. The codewords in the zigzag, unmasked, split into blocks, each
   corrected (qr/ReedSolomon: syndromes, Berlekamp-Massey, Chien
   search, Forney), and the segments decoded (numeric, alphanumeric, byte;
   an ECI skipped). A failed grid is tried transposed: a code seen from
   behind.

`compiler/tests/cases/1288_qr_reader` reads codes drawn by
`qrDrawGray` (antialiased, through any four corners): straight, 3 and 12
pixel modules, rotated 30 and 135 degrees, tilted, steeply tilted (a plate
seen from above), blurred with noise, under uneven light, mirrored,
versions 1 to 25; damage within each level's error correction is corrected
(3, 7, 10 and 14 codewords at L, M, Q, H) and beyond it gives no result
(never a wrong text); noise, a gradient and a checkerboard give none.

Time (Apple M1 Max, release build): a 512 x 512 image with a code reads in
about 1.3 ms; one with no code is turned down in 3.5-7 ms. Scanning a part
of the rendered frame every few frames costs little.

## Scanning the rendered frame

`lib/gpu/FrameRegionReader.rae` reads a rectangle of the rendered frame back
from the GPU without waiting: after present, the canvas's offscreen target
holds the composed frame; the rectangle is copied into a staging buffer
(`recordCopyTextureRegionToBuffer`, lib/gpu/GpuRenderTransfer) and
submitted, and the following frames poll the submission, map the buffer and
poll the map, so the pixels arrive two frames later and nothing stalls.
While a reader is in use the frame renders into the offscreen target rather
than straight into the drawable (one extra full-frame copy per frame).

`qr/qrScanSystem` (`QrScanner`) turns that into an in-game camera:
every `intervalFrames` frames a `regionSide` square in the middle of the
frame is read back, turned to gray and decoded; `scanner.latest` holds the
last code read. The app calls `qrScanFrame(scanner:, canvas:)` after each
present. Example 147 does it with `RAE_QR_SCAN=1` (every 10 frames, logged).

In a game on the walker kit that uses this package and holds a `WalkerQrScan`
(`qr/WalkerQrScan`: `createWalkerQrScan` in `walkerGameStart`, `walkerQrUpdate`
in `walkerGameUpdate`, `walkerQrRelease` in `walkerGameEnd`; example 148),
`RAE_WALKER_QR=1` puts a 1 m cube with a code on every side 10 m ahead of the
walker (a physics cube: it can be pushed) and scans the middle of the frame
every 10 frames, logging what it reads. `RAE_WALKER_QR_TEST=1` sweeps the camera
round the cube at eye height and reports how far each angle still reads, then
ends. The walkers with no game (136, 137, 143) do not have these settings.
148 gave the same sweep result before and after this code moved to the package
(2026-11-05), reading 4 to 12 m straight on, a smaller range at an angle, and
nothing at +55 degrees. Measured earlier in 143 (2560 x 1600 frame, a 512 x 512
scan square, noon, 2026-11-03):

| angle off the face | reads from | to |
|---|---|---|
| 0 degrees | 4 m | 12 m |
| +30 degrees | 4 m | 8 m |
| -30 degrees | 5 m | 10 m |
| +55 degrees | 5 m | 6 m |
| -55 degrees | 4 m | 5 m |

Closer than 4 m the code is larger than the scan square. The far limit is
the code's module size on screen (the version 3 code's 29 modules over
0.78 m); a larger cube or a bigger scan square reads farther. Two things
the sweep showed and the reader now handles: the alignment pattern is
checked as a whole (dark ring, light ring, dark middle, both ways), not as
any light-dark-light, which a data module also is, and when the grid does
not decode with the alignment pattern found it is tried with a looser
search and then without one. Each scan's pixels arrive 2 frames after the
copy, and its decode takes about 2 ms.
