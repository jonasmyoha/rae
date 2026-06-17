#!/bin/bash
# Regenerate the Roboto MTSDF atlas used by this example.
#
# Requires Chlumsky's `msdf-atlas-gen` on $PATH:
#   git clone --recurse-submodules https://github.com/Chlumsky/msdf-atlas-gen \
#       /tmp/msdf-atlas-gen
#   cd /tmp/msdf-atlas-gen && mkdir -p build && cd build
#   cmake -DCMAKE_BUILD_TYPE=Release -DMSDF_ATLAS_USE_VCPKG=OFF \
#         -DMSDF_ATLAS_USE_SKIA=OFF .. && make -j4
#   # then add build/bin to PATH or point MAG at the binary below.
#
# `-pxrange 12` is deliberately wide (the stdlib default bake uses 4).
# The clean outline width scales with pxrange — at `size/8` px on screen
# per unit of render size — so 12 lets the example show a genuinely
# THICK outline (~8px on the 72px sample) without the distance field
# saturating into a dark box. 4 would cap the outline near ~2px.

set -euo pipefail
cd "$(dirname "$0")/../assets"

MAG="${MAG:-msdf-atlas-gen}"
if ! command -v "$MAG" >/dev/null 2>&1 && [ ! -x "$MAG" ]; then
  echo "error: msdf-atlas-gen not found (set MAG=/path/to/msdf-atlas-gen)" >&2
  exit 1
fi

"$MAG" \
  -font Roboto-Regular.ttf \
  -type mtsdf \
  -format png \
  -imageout Roboto-Regular.mtsdf.png \
  -json Roboto-Regular.mtsdf.json \
  -size 48 \
  -pxrange 12 \
  -potr

echo "Regenerated Roboto MTSDF atlas (pxrange 12)."
