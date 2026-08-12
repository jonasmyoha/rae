#!/bin/bash
# Bake 113's Roboto MTSDF atlas.
#
# The CHARSET is 110's (tools/text-glyphs.txt over there), deliberately shared:
# both examples mount the same lib/app3d panel, so a glyph one of them needs the
# other needs too. Two charset files would drift, and the failure is silent —
# a missing glyph is skipped, so the text just loses a letter.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ASSETS="$HERE/../assets"
FONT="$HERE/../../106_mobile_ui/assets/Roboto-Regular.ttf"
CHARSET="$HERE/../../110_gpu3d_ui/tools/text-glyphs.txt"
GEN="${MSDF_ATLAS_GEN:-msdf-atlas-gen}"

if ! command -v "$GEN" >/dev/null 2>&1 && [ ! -x "$GEN" ]; then
  echo "error: msdf-atlas-gen not found (set MSDF_ATLAS_GEN to its path)" >&2
  exit 1
fi
command -v magick >/dev/null 2>&1 || { echo "error: ImageMagick magick not found" >&2; exit 1; }

"$GEN" -font "$FONT" -charset "$CHARSET" \
  -type mtsdf -format png \
  -imageout "$ASSETS/Roboto-Regular.mtsdf.png" \
  -json "$ASSETS/Roboto-Regular.mtsdf.json" \
  -size 48 -pxrange 12 -potr

magick "$ASSETS/Roboto-Regular.mtsdf.png" -depth 8 RGBA:"$ASSETS/Roboto-Regular.mtsdf.raw"
rm "$ASSETS/Roboto-Regular.mtsdf.png"
echo "Baked 113 Roboto atlas from 110's shared charset."
