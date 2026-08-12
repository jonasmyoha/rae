#!/bin/bash
# Bake 110's Roboto MTSDF atlas, including the non-ASCII UI glyphs in text-glyphs.txt.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ASSETS="$HERE/../assets"
FONT="$HERE/../../106_mobile_ui/assets/Roboto-Regular.ttf"
CHARSET="$HERE/../../../tools/text-glyphs.txt"
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
echo "Baked 110 Roboto atlas with ASCII and degree-symbol glyphs."
