#!/bin/sh
# Fixed silver-birch studies. Full frames and logs remain outside git.
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
output="${RAE_BIRCH_OUTPUT:-/tmp/rae-birch-studies}"
mode="${1:-captures}"
geometry="$root/.rae/apps/138_procgen_showcase/window.138_procgen_showcase.geometry"
fixedFrames=60
fixedDelta=0.016666667

mkdir -p "$output" "$output/saved-state"
geometryHad=0
if [ -f "$geometry" ]; then
  cp "$geometry" "$output/saved-state/grove.geometry"
  geometryHad=1
fi

restoreState() {
  if [ "$geometryHad" -eq 1 ]; then
    cp "$output/saved-state/grove.geometry" "$geometry"
  elif [ -f "$geometry" ]; then
    python3 -c 'import os, sys; os.unlink(sys.argv[1])' "$geometry"
  fi
}
trap restoreState EXIT INT TERM

mkdir -p "$(dirname "$geometry")"
printf '%s\n' '100 100 1280 800' > "$geometry"

capture() {
  view="$1"
  name="$2"
  env RAE_TREELAB_STUDY="$view" RAE_DEBUG_OVERLAY=0 RAE_UI_HEADLESS=1 \
    RAE_HEADLESS_FRAMES="$fixedFrames" RAE_FIXED_DT="$fixedDelta" \
    RAE_GPU2D_SCREENSHOT="$output/$name.bmp" \
    perl -e 'alarm shift; exec @ARGV' 120 "$root/compiler/bin/rae" run \
      --project "$root/examples/146_treeLab" \
      "$root/examples/146_treeLab/Main.rae" > "$output/$name.log" 2>&1
  sips -s format png -z 800 1280 "$output/$name.bmp" --out "$output/$name.png" >/dev/null
}

writeManifest() {
  cat > "$output/manifest.txt" <<EOF
Rae silver-birch studies
commit: $(git -C "$root" rev-parse HEAD)
window: 1280x800 points; PNG: 1280x800 pixels
capture clock: 60 frames at 0.016666667 s; 1.0 simulated second
renderer: deferred, toon, two-sided foliage, cascaded shadows, SSAO, SMAA, bloom
species: silver birch; seeds 1437/311/4201/827/2189; heights 7.1/8.0/6.6/7.6/8.4 m

birch-day: birches preset; Preetham 10:00
birch-side: birches-side preset; Preetham 10:00
birch-under: birches-under preset; Preetham 10:00
birch-warm: birches-warm preset; Preetham 17:15
birch-cool: birches-cool preset; Preetham 07:15
birch-night: birches-night preset; Preetham 23:00 with moon
EOF
}

captureSet() {
  capture birches birch-day
  capture birches-side birch-side
  capture birches-under birch-under
  capture birches-warm birch-warm
  capture birches-cool birch-cool
  capture birches-night birch-night
  writeManifest
}

verifyStable() {
  capture birches-side verify-side-a
  capture birches-side verify-side-b
  cmp "$output/verify-side-a.bmp" "$output/verify-side-b.bmp"
}

case "$mode" in
  captures) captureSet ;;
  verify) verifyStable ;;
  all) captureSet; verifyStable ;;
  *) printf '%s\n' "usage: $0 [captures|verify|all]" >&2; exit 2 ;;
esac

printf '%s\n' "birch study output: $output"
