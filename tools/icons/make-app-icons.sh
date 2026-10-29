#!/bin/bash
# make-app-icons.sh — the featured examples' macOS app icons from their
# screenshots (docs/screenshots/<example>.png): a square crop of each one's
# subject, in a rounded square with the macOS icon margin (824 of 1024),
# 512 x 512, written to examples/<example>/appIcon.png. Gpu2d.initWindow and
# Sdl3.initWindow set it as the Dock icon (lib/Gpu2dWindowIcon.rae).
# Needs ImageMagick (magick). Run from the repository root.
set -euo pipefail
size=512
inner=412           # 824 of 1024
radius=92           # about 22% of the inner square
# example  centerX centerY side   (parts of the width, height, height;
# side "fit": the whole picture scaled into the square, its top and
# bottom edges' colors filling above and below, for a wide game field such as Pong's)
crops="
102_gpu2d_animated      0.62 0.45 0.62
106_mobile_ui           0.50 0.33 0.40
121_ui_editor           0.50 0.36 0.56
123_physics_playground_c 0.62 0.48 0.62
125_vehicle_c           0.50 0.62 0.62
134_metaballs_deferred  0.63 0.50 0.70
136_walker_character    0.56 0.58 0.78
138_procgen_showcase    0.52 0.50 0.80
141_ocean_fft           0.50 0.58 0.80
53_raytracer_webgpu_text 0.50 0.52 0.74
91_pong                 0.50 0.50 fit
94_tetris2d             0.50 0.55 0.66
143_walker_forest       0.45 0.55 0.95
"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
magick -size ${inner}x${inner} xc:none -fill white -draw "roundrectangle 0,0 $((inner-1)),$((inner-1)) $radius,$radius" "$work/mask.png"
echo "$crops" | while read -r name cx cy side; do
  [ -z "$name" ] && continue
  src="docs/screenshots/$name.png"
  w=$(magick identify -format %w "$src"); h=$(magick identify -format %h "$src")
  if [ "$side" = fit ]; then
    top=$(magick "$src" -format '%[pixel:p{0,0}]' info:)
    bottom=$(magick "$src" -format "%[pixel:p{0,$((h-1))}]" info:)
    magick "$src" -resize ${inner}x${inner} "$work/fit.png"
    fh=$(magick identify -format %h "$work/fit.png")
    pad=$(( (inner - fh) / 2 ))
    magick "$work/fit.png" -background "$top" -gravity south -extent ${inner}x$((fh + pad)) \
      -background "$bottom" -gravity north -extent ${inner}x${inner} "$work/crop.png"
  else
    s=$(python3 -c "print(int(min($side*$h, $w, $h)))")
    x=$(python3 -c "print(max(0, min($w-$s, int($cx*$w-$s/2))))")
    y=$(python3 -c "print(max(0, min($h-$s, int($cy*$h-$s/2))))")
    magick "$src" -crop ${s}x${s}+${x}+${y} +repage -resize ${inner}x${inner}! "$work/crop.png"
  fi
  magick "$work/crop.png" "$work/mask.png" -alpha off -compose CopyOpacity -composite "$work/round.png"
  magick -size ${size}x${size} xc:none "$work/round.png" -gravity center -composite "examples/$name/appIcon.png"
  echo "examples/$name/appIcon.png"
done
