#!/bin/sh
# App icons from a shot of the app (a face, a logo, whatever the app frames):
# crops a square round a point of it, brightens it toward a measured mean
# lightness (a gamma on Oklab's L, so the hue keeps as it lightens), refuses
# it if it lands outside the band, loses its colour or shifts its hue, and
# writes every size the platforms want:
#
#   <icons>/ios/AppIcon-1024.png        the iOS icon (Xcode makes the rest;
#                                       tools/ios/gen-ios.sh's RAE_IOS_ICON)
#   <icons>/android/playstore-512.png   the Play Store listing icon
#   <icons>/android/mipmap-*/           ic_launcher.png, ic_launcher_round.png
#                                       and the adaptive icon's foreground
#   <icons>/android/mipmap-anydpi-v26/  the adaptive icon (foreground + background)
#   <icons>/macos/AppIcon-512.png       a rounded square on Apple's macOS grid
#                                       (Gpu2dWindowIcon.setWindowIcon)
#
#   tools/ios/make-icons.sh <shot image> <icons dir>
#
# Settings (environment; per mille of the shot's width or height):
#   ICON_CENTRE_X, ICON_CENTRE_Y   the square's middle (504, 520)
#   ICON_TIGHT_HALF                half the tight square, of the height (329):
#                                  iOS, the Play Store, Android's legacy icons
#   ICON_WIDE_HALF                 half the wide one (362): Android's adaptive
#                                  foreground, whose mask shows its middle
#   ICON_LIGHTNESS                 the target mean CIELab lightness (0.56), in
#   ICON_LIGHTNESS_LOW / _HIGH     the band it must land in (0.53 / 0.60)
#   ICON_SATURATION_LOW            the least HSL saturation left (0.40)
#   ICON_HUE_PATCH                 "w h x y" as shares of the crop: a patch of
#                                  one colour whose Oklch hue must not shift
#                                  more than 0.05 ("0.08 0.05 0.46 0.27")
#   ICON_BACKGROUND                Android's adaptive background (#DCE3EC)
# Needs ImageMagick (brew install imagemagick).
set -eu
command -v magick >/dev/null || { echo "make-icons: needs ImageMagick (brew install imagemagick)" >&2; exit 1; }
[ $# -eq 2 ] || { echo "usage: $0 <shot image> <icons dir>" >&2; exit 2; }
shot=$1
icons=$2
centreXPerMille=${ICON_CENTRE_X:-504}
centreYPerMille=${ICON_CENTRE_Y:-520}
tightHalf=${ICON_TIGHT_HALF:-329}
wideHalf=${ICON_WIDE_HALF:-362}
hueW=$(echo "${ICON_HUE_PATCH:-0.08 0.05 0.46 0.27}" | cut -d' ' -f1)
hueH=$(echo "${ICON_HUE_PATCH:-0.08 0.05 0.46 0.27}" | cut -d' ' -f2)
hueX=$(echo "${ICON_HUE_PATCH:-0.08 0.05 0.46 0.27}" | cut -d' ' -f3)
hueY=$(echo "${ICON_HUE_PATCH:-0.08 0.05 0.46 0.27}" | cut -d' ' -f4)
background=${ICON_BACKGROUND:-#DCE3EC}
scratch=${TMPDIR:-/tmp}/rae-icon-$$

width=$(magick identify -format '%w' "$shot")
height=$(magick identify -format '%h' "$shot")
# The middle, and two squares round it: a tight one (iOS, the Play Store,
# Android's legacy icons) and a wider one (Android's adaptive foreground,
# whose mask shows only its middle two thirds).
centreX=$((width * centreXPerMille / 1000))
centreY=$((height * centreYPerMille / 1000))
crop() { # half-size (per mille of the height) -> out
  half=$((height * $1 / 1000))
  magick "$shot" -crop "$((half * 2))x$((half * 2))+$((centreX - half))+$((centreY - half))" +repage \
    -alpha off "$2"
}
mkdir -p "$icons/ios" "$icons/android"
tight=$scratch-tight.png
wide=$scratch-wide.png
crop "$tightHalf" "$tight"
crop "$wideHalf" "$wide"

# Colour correction: a shot is often dark for an icon (~0.45). Lift
# the L channel of Oklab with a gamma curve — the midtones rise,
# black and white stay, and a/b are untouched; Oklab keeps the hue as it
# lightens (in CIELab a blue drifts toward violet) — stepping the gamma up
# until the mean lightness (CIELab L) reaches the target. Then check it is
# inside the band and still saturated, or fail.
lightness() { magick "$1" -colorspace Lab -channel R -separate +channel -format '%[fx:mean]' info:; }
saturation() { magick "$1" -colorspace HSL -channel G -separate +channel -format '%[fx:mean]' info:; }
# The Oklch hue of the ICON_HUE_PATCH, in the crop's own terms (lifting
# CIELab L instead can turn a blue ~0.25 toward violet; Oklab keeps it within
# ~0.001)
patchHue() {
  magick "$1" -crop "%[fx:w*$hueW]x%[fx:h*$hueH]+%[fx:w*$hueX]+%[fx:h*$hueY]" +repage \
    -colorspace Oklch -format '%[fx:mean.b]' info:
}
brighten() { # gamma, in, out
  magick "$2" -colorspace Oklab -channel R -gamma "$1" +channel -colorspace sRGB -alpha off "$3"
}
targetLightness=${ICON_LIGHTNESS:-0.56}
lowestLightness=${ICON_LIGHTNESS_LOW:-0.53}
highestLightness=${ICON_LIGHTNESS_HIGH:-0.60}
lowestSaturation=${ICON_SATURATION_LOW:-0.40}
before=$(lightness "$tight")
corrected=$scratch-corrected.png
gamma=1.00
for candidate in 1.00 1.05 1.10 1.15 1.20 1.25 1.30 1.35 1.40 1.45 1.50 1.60 1.70 1.80; do
  gamma=$candidate
  brighten "$gamma" "$tight" "$corrected"
  awk -v l="$(lightness "$corrected")" -v t="$targetLightness" 'BEGIN { exit !(l >= t) }' && break
done
after=$(lightness "$corrected")
saturated=$(saturation "$corrected")
hueBefore=$(patchHue "$tight")
hueAfter=$(patchHue "$corrected")
echo "make-icons: lightness $before -> $after (gamma $gamma; band $lowestLightness-$highestLightness), saturation $(saturation "$tight") -> $saturated (at least $lowestSaturation), patch hue $hueBefore -> $hueAfter"
awk -v l="$after" -v lo="$lowestLightness" -v hi="$highestLightness" -v s="$saturated" -v sl="$lowestSaturation" \
  -v h0="$hueBefore" -v h1="$hueAfter" \
  'BEGIN { d = h1 - h0; if (d < 0) d = -d; exit !(l >= lo && l <= hi && s >= sl && d < 0.05) }' \
  || { echo "make-icons: the corrected icon is outside the band, washed out or off-hue — not written" >&2; exit 1; }
cp "$corrected" "$tight"
brighten "$gamma" "$wide" "$wide"

magick "$tight" -resize 1024x1024 -alpha off "$icons/ios/AppIcon-1024.png"
magick "$tight" -resize 512x512 -alpha off "$icons/android/playstore-512.png"
for density in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
  name=${density%%:*}; size=${density##*:}
  dir="$icons/android/mipmap-"$name
  mkdir -p "$dir"
  magick "$tight" -resize "${size}x${size}" -alpha off "$dir/ic_launcher.png"
  magick "$tight" -resize "${size}x${size}" \
    \( -size "${size}x${size}" xc:none -fill white -draw "circle $((size / 2)),$((size / 2)) $((size / 2)),0" \) \
    -compose DstIn -composite "$dir/ic_launcher_round.png"
  # The adaptive layer is 108 dp for the 72 dp icon: 2.25x the legacy size
  layer=$((size * 9 / 4))
  magick "$wide" -resize "${layer}x${layer}" -alpha off "$dir/ic_launcher_foreground.png"
done
mkdir -p "$icons/android/mipmap-anydpi-v26" "$icons/android/values"
for name in ic_launcher ic_launcher_round; do
  cat > "$icons/android/mipmap-anydpi-v26/$name.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
  <background android:drawable="@color/ic_launcher_background" />
  <foreground android:drawable="@mipmap/ic_launcher_foreground" />
</adaptive-icon>
EOF
done
# Behind the foreground (which is opaque edge to edge, so this only shows
# while a launcher animates the mask)
cat > "$icons/android/values/ic_launcher_background.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<resources>
  <color name="ic_launcher_background">$background</color>
</resources>
EOF
# macOS: a Dock icon an app can set itself at start (Gpu2dWindowIcon; a bare
# executable has no bundle to take one from). Apple's macOS grid: the
# icon's body is a rounded square 824/1024 of the canvas, its corners 185/1024,
# with transparent room round it, not iOS's full-bleed square.
mkdir -p "$icons/macos"
magick "$icons/ios/AppIcon-1024.png" -resize 412x412 -alpha set \
  \( -size 412x412 xc:none -fill white -draw "roundrectangle 0,0 411,411 92,92" \) \
  -compose DstIn -composite \
  -compose over -background none -gravity center -extent 512x512 "$icons/macos/AppIcon-512.png"
rm -f "$tight" "$wide" "$corrected"
echo "make-icons: wrote "$icons/ios", "$icons/android" and "$icons/macos" from $shot (${width}x${height})"
