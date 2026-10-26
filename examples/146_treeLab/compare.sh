#!/usr/bin/env bash
# The tree lab's comparison picture (docs/reference-tree-plan.md, "How every
# step is judged"): reference crop T02 on the left, the lab's render on the
# right, the same height. The render is the middle strip of the frame in
# T02's proportions (330 x 570), so the near camera's framing is compared
# with T02's framing as it is.
#
#   examples/146_treeLab/compare.sh [near|mid|far]
#
# TREELAB_T02   the crop (default: the private reference study's T02.png,
#               in the looks-development repository)
# TREELAB_OUT   the picture to write (default: treeReference/lab/ in
#               looks-development, compare-<camera>.png, not committed there). T02 is private, so
#               the picture is never written into this repository.
# TREELAB_ITERATION  a round's name (YYYY-MM-DD-stepN-short-name): also copy
#               the picture into treeReference/rounds/<name>/, the history
#               that is never overwritten (looks-development's README)
# Needs macOS `sips` (crop, scale, PNG/BMP) and python3 (joins two BMPs).
set -u
cd "$(dirname "$0")/../.." || exit 1     # -> the repository root
CAMERA="${1:-near}"
LOOKS="${TREELAB_LOOKS_REPO:-$HOME/Documents/my_own_stuff/own_repos/looks-development}"
T02="${TREELAB_T02:-$LOOKS/references/crops/T02.png}"
OUT="${TREELAB_OUT:-$LOOKS/treeReference/lab/compare-$CAMERA.png}"
[ -f "$T02" ] || { echo "error: reference crop not found: $T02 (set TREELAB_T02)" >&2; exit 1; }
case "$OUT" in "$PWD"/*|examples/*|lib/*|docs/*)
  echo "error: $OUT is inside the repository; T02 is private (set TREELAB_OUT elsewhere)" >&2; exit 1;;
esac
TMP="$(mktemp -d -t rae_treelab_XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

# 1. The render, from the chosen camera
RAE_TREELAB_CAMERA="$CAMERA" RAE_SDL_HEADLESS_MS=4000 RAE_GPU2D_SCREENSHOT="$TMP/render.bmp" \
  perl -e 'alarm shift; exec @ARGV' 240 compiler/bin/rae run examples/146_treeLab/Main.rae \
  > "$TMP/run.log" 2>&1
[ -f "$TMP/render.bmp" ] || { echo "error: no render (see the run's log below)" >&2; tail -20 "$TMP/run.log" >&2; exit 1; }

# 2. The middle strip in T02's proportions, both scaled to one height
HEIGHT=1140
WIDTH=660
read -r RW RH < <(sips -g pixelWidth -g pixelHeight "$TMP/render.bmp" | awk '/pixelWidth/{w=$2}/pixelHeight/{h=$2}END{print w, h}')
STRIP=$(( RH * 330 / 570 ))
LEFT=$(( (RW - STRIP) / 2 ))
sips -c "$RH" "$STRIP" --cropOffset 0 "$LEFT" "$TMP/render.bmp" --out "$TMP/strip.bmp" > /dev/null
sips -z "$HEIGHT" "$WIDTH" -s format bmp "$TMP/strip.bmp" --out "$TMP/right.bmp" > /dev/null
sips -z "$HEIGHT" "$WIDTH" -s format bmp "$T02" --out "$TMP/left.bmp" > /dev/null

# 3. Side by side, a 16-pixel dark gap between
python3 - "$TMP/left.bmp" "$TMP/right.bmp" "$TMP/joined.bmp" <<'PY'
import struct, sys

def read_bmp(path):
    data = open(path, 'rb').read()
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<ii', data, 18)
    bits = struct.unpack_from('<H', data, 28)[0]
    step = bits // 8
    row = (width * step + 3) // 4 * 4
    top_down = height < 0
    height = abs(height)
    rows = []
    for y in range(height):
        source = y if top_down else height - 1 - y
        start = offset + source * row
        rows.append(bytes(b for x in range(width) for b in data[start + x * step:start + x * step + 3]))
    return width, height, rows

left_w, left_h, left = read_bmp(sys.argv[1])
right_w, right_h, right = read_bmp(sys.argv[2])
gap = 16
height = min(left_h, right_h)
width = left_w + gap + right_w
row = (width * 3 + 3) // 4 * 4
pixels = bytearray()
for y in range(height - 1, -1, -1):
    line = left[y] + bytes([24, 25, 22]) * gap + right[y]
    pixels += line + bytes(row - len(line))
header = struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54)
info = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
open(sys.argv[3], 'wb').write(header + info + pixels)
PY
mkdir -p "$(dirname "$OUT")"
sips -s format png "$TMP/joined.bmp" --out "$OUT" > /dev/null
echo "wrote $OUT (left: T02, right: camera '$CAMERA')"
if [ -n "${TREELAB_ITERATION:-}" ]; then
  ROUND="$LOOKS/treeReference/rounds/$TREELAB_ITERATION"
  mkdir -p "$ROUND"
  cp "$OUT" "$ROUND/compare-$CAMERA.png"
  echo "kept $ROUND/compare-$CAMERA.png (add the round to treeReference/rounds.js and commit looks-development)"
fi
