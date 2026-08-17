#!/usr/bin/env bash
# Bake the Material-Icons MSDF atlas used by the gpu2d UI backend (#145).
#
# The gpu2d renderer has no runtime font rasterizer (unlike raylib), so every
# font it draws — including the Material icon glyphs (mat:*) — must be baked to
# an MSDF atlas offline. This produces MaterialIconsRound-Regular.mtsdf.{json,raw}
# in the same format as the committed Roboto atlas (type mtsdf, distanceRange 12,
# size 48, 512x512, yOrigin bottom), so sdf_text.loadSdfFont loads it identically.
#
# Requires: msdf-atlas-gen (https://github.com/Chlumsky/msdf-atlas-gen, built
# from source) and ImageMagick (`magick`). The charset is the union of icon
# codepoints in lib/ui/icon_codepoints.rae.
set -euo pipefail
cd "$(dirname "$0")/../../.."          # repo root (rae/)
GEN="${MSDF_ATLAS_GEN:-msdf-atlas-gen}"
A=examples/98_mobile_ui/assets/MaterialIconsRound-Regular
CHARSET=$(grep -oE 'u\{[0-9a-fA-F]+\}' lib/ui/icon_codepoints.rae \
  | sed -E 's/u\{([0-9a-fA-F]+)\}/0x\1/' | sort -u | tr '\n' ',' | sed 's/,$//')
echo "charset: $CHARSET"
"$GEN" -font "$A.otf" -charset <(printf '%s' "$CHARSET") \
  -type mtsdf -format png -imageout "$A.mtsdf.png" -json "$A.mtsdf.json" \
  -size 48 -pxrange 12 -dimensions 512 512 -yorigin bottom
# PNG -> headerless RGBA8 (the .raw sdf_text.loadAtlas fread()s). Byte-identical
# to how the committed Roboto .raw was produced.
magick "$A.mtsdf.png" -depth 8 RGBA:"$A.mtsdf.raw"
echo "baked $A.mtsdf.{json,raw,png}"
