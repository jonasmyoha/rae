#!/bin/sh
# The tree lab's season check: the One tree scene at latitude 60 in summer (day 180), autumn (290)
# and winter (20), three headless pictures that must differ (the crown turning, then bare with
# snow). Exit 0 when each pair differs by a mean of at least 5 levels in 0..255.
#   sh examples/146_treeLab/seasonCheck.sh
set -u
cd "$(dirname "$0")/../.." || exit 1     # -> the repository root
RAE="${RAE_BIN:-compiler/bin/rae}"
OUT="${TREELAB_SEASON_OUT:-/tmp/treelab-season}"
mkdir -p "$OUT"
for day in 180 290 20; do
  RAE_TREELAB_SCENE=onetree RAE_DAY_OF_YEAR=$day RAE_LATITUDE=60 RAE_SDL_HEADLESS_MS=6000 \
    RAE_GPU2D_SCREENSHOT="$OUT/day$day.bmp" perl -e 'alarm shift; exec @ARGV' 400 \
    "$RAE" run examples/146_treeLab/Main.rae > "$OUT/day$day.log" 2>&1 || { echo "FAIL: day $day did not run"; exit 1; }
done
python3 -I - "$OUT" <<'P'
import struct, sys
def load(p):
    b = open(p, 'rb').read()
    off = struct.unpack_from('<I', b, 10)[0]
    w, h = struct.unpack_from('<ii', b, 18)
    return b, off, w * abs(h), struct.unpack_from('<H', b, 28)[0] // 8
def mean(a, b):
    (x, ox, n, bpp), (y, oy, _, _) = load(a), load(b)
    total = 0; count = 0
    for i in range(0, n, 7):
        total += max(abs(x[ox + i * bpp + k] - y[oy + i * bpp + k]) for k in range(3)); count += 1
    return total / count
out = sys.argv[1]; ok = True
for a, b in ((180, 290), (290, 20), (180, 20)):
    d = mean(f"{out}/day{a}.bmp", f"{out}/day{b}.bmp")
    print(f"day {a} vs day {b}: mean difference {d:.1f}")
    ok = ok and d >= 5.0
print("PASS: the One tree scene follows the date" if ok else "FAIL: a season did not change the picture")
sys.exit(0 if ok else 1)
P
