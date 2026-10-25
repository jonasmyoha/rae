#!/bin/sh
# Fixed Autumn Grove and Huge Forest comparison captures. Output stays under
# /tmp by default; only the small curated contact sheet belongs in git.
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
output="${RAE_LANDSCAPE_OUTPUT:-/tmp/rae-landscape-comparisons}"
mode="${1:-captures}"
fixedFrames=60
fixedDelta=0.016666667
groveGeometry="$root/.rae/apps/138_procgen_showcase/window.138_procgen_showcase.geometry"
forestGeometry="$root/.rae/apps/143_walker_forest/window.143_walker_forest.geometry"
forestState="$root/.rae/apps/143_walker_forest/state.143_walker_forest.json"

mkdir -p "$output" "$output/saved-state"

groveGeometryHad=0
forestGeometryHad=0
forestStateHad=0
if [ -f "$groveGeometry" ]; then
  cp "$groveGeometry" "$output/saved-state/grove.geometry"
  groveGeometryHad=1
fi
if [ -f "$forestGeometry" ]; then
  cp "$forestGeometry" "$output/saved-state/forest.geometry"
  forestGeometryHad=1
fi
if [ -f "$forestState" ]; then
  cp "$forestState" "$output/saved-state/forest.state"
  forestStateHad=1
fi

restoreState() {
  if [ "$groveGeometryHad" -eq 1 ]; then
    cp "$output/saved-state/grove.geometry" "$groveGeometry"
  elif [ -f "$groveGeometry" ]; then
    python3 -c 'import os, sys; os.unlink(sys.argv[1])' "$groveGeometry"
  fi
  if [ "$forestGeometryHad" -eq 1 ]; then
    cp "$output/saved-state/forest.geometry" "$forestGeometry"
  elif [ -f "$forestGeometry" ]; then
    python3 -c 'import os, sys; os.unlink(sys.argv[1])' "$forestGeometry"
  fi
  if [ "$forestStateHad" -eq 1 ]; then
    cp "$output/saved-state/forest.state" "$forestState"
  elif [ -f "$forestState" ]; then
    python3 -c 'import os, sys; os.unlink(sys.argv[1])' "$forestState"
  fi
}
trap restoreState EXIT INT TERM

mkdir -p "$(dirname "$groveGeometry")" "$(dirname "$forestGeometry")"
printf '%s\n' '100 100 1280 800' > "$groveGeometry"
printf '%s\n' '100 100 1280 800' > "$forestGeometry"
# An empty file means no walker hot-reload state. The fixed camera is therefore
# applied to the same fresh world on every run.
printf '%s' '' > "$forestState"

captureGrove() {
  view="$1"
  name="$2"
  camera="${3:-}"
  env RAE_GROVE_VIEW="$view" RAE_CAMERA_AT="$camera" RAE_DEBUG_OVERLAY=0 \
    RAE_UI_HEADLESS=1 RAE_HEADLESS_FRAMES="$fixedFrames" RAE_FIXED_DT="$fixedDelta" \
    RAE_GPU2D_SCREENSHOT="$output/$name.bmp" \
    perl -e 'alarm shift; exec @ARGV' 120 "$root/compiler/bin/rae" run \
      --project "$root/examples/138_procgen_showcase" \
      "$root/examples/138_procgen_showcase/Main.rae" > "$output/$name.log" 2>&1
  sips -s format png -z 800 1280 "$output/$name.bmp" --out "$output/$name.png" >/dev/null
}

captureForest() {
  name="$1"
  camera="$2"
  env RAE_CAMERA_AT="$camera" RAE_WALKER_HOUR=12 RAE_WALKER_DAY=298 RAE_DRS=off \
    RAE_DEBUG_OVERLAY=0 RAE_UI_HEADLESS=1 RAE_HEADLESS_FRAMES="$fixedFrames" \
    RAE_FIXED_DT="$fixedDelta" RAE_GPU2D_SCREENSHOT="$output/$name.bmp" \
    perl -e 'alarm shift; exec @ARGV' 150 "$root/compiler/bin/rae" run \
      --project "$root/examples/143_walker_forest" \
      "$root/examples/143_walker_forest/Main.rae" > "$output/$name.log" 2>&1
  sips -s format png -z 800 1280 "$output/$name.bmp" --out "$output/$name.png" >/dev/null
}

writeManifest() {
  forestCount="$(sed -n 's/^\[forest\] \([0-9][0-9]*\) trees planted$/\1/p' "$output/143-walk-00.log" | head -n 1)"
  cat > "$output/manifest.txt" <<EOF
Rae landscape comparison views
commit: $(git -C "$root" rev-parse HEAD)
window: 1280x800 points; PNG: 1280x800 pixels
capture clock: 60 frames at 0.016666667 s; 1.0 simulated second
138 sun/time: Preetham, hour 10:00, clock paused; exposure 1.0
138 effects: deferred, toon on, cascaded shadows on, SSAO, SMAA, bloom, TAA off, GPU grass and wind on
138 instances: 63 trees; 21 generated bushes; 7 scarlet shrubs
138 generation seeds: legacy trees 501/605/710/301/903/217; fluffy prototypes 73; woodland 4177; camphor 219; birch 1437; rock groups 230/247/311/359/401/443/487/521/563/601
143 sun/time: Preetham, day 298, hour 12:00 start; exposure 1.0
143 effects: deferred, toon on, cascaded shadows on, SSAO, SMAA, bloom, TAA off, DRS off, FFT water, GPU grass and wind on
143 placement seeds: density 401; positions 811/812; keep 813; yaw 814; species 733/977/131
143 forest: $forestCount trees

138-roots: preset roots; bare camphor wood, roots and foliage underside
138-trees-side: preset trees; legacy maple/date-palm side view
138-foliage-close: preset fluffy; small-leaved-lime foliage close view
138-birch-backlit: preset birches plus camera 0 2.5 0.1 0 22 0 0 0 -19 -10 0 0
138-rocks: preset rocks; path-side rock group
143-ground-cover: camera 1 0 0.24 -0.20 4.2 0 18.0 0.35 0 0 0 0
143-walk-00: camera 1 0 0.24 -0.06 4.2 0 -18 2.4 0 0 0 0
143-walk-01: camera 1 0 0.24 -0.06 4.2 0 -5 2.4 0 0 0 0
143-walk-02: camera 1 0 0.24 -0.06 4.2 0 8 2.4 0 0 0 0
EOF
}

captureSet() {
  captureGrove roots 138-roots
  captureGrove trees 138-trees-side
  captureGrove fluffy 138-foliage-close
  captureGrove birches 138-birch-backlit '0 2.5 0.1 0 22 0 0 0 -19 -10 0 0'
  captureGrove rocks 138-rocks
  captureForest 143-ground-cover '1 0 0.24 -0.20 4.2 0 18.0 0.35 0 0 0 0'
  captureForest 143-walk-00 '1 0 0.24 -0.06 4.2 0 -18 2.4 0 0 0 0'
  captureForest 143-walk-01 '1 0 0.24 -0.06 4.2 0 -5 2.4 0 0 0 0'
  captureForest 143-walk-02 '1 0 0.24 -0.06 4.2 0 8 2.4 0 0 0 0'
  writeManifest
}

verifyStable() {
  captureGrove roots verify-138-a
  captureGrove roots verify-138-b
  cmp "$output/verify-138-a.bmp" "$output/verify-138-b.bmp"
  captureForest verify-143-a '1 0 0.24 -0.06 4.2 0 -5 2.4 0 0 0 0'
  captureForest verify-143-b '1 0 0.24 -0.06 4.2 0 -5 2.4 0 0 0 0'
  cmp "$output/verify-143-a.bmp" "$output/verify-143-b.bmp"
}

captureMetrics() {
  env RAE_GROVE_VIEW=grove RAE_DEBUG_OVERLAY=0 RAE_UI_HEADLESS=1 \
    RAE_SDL_HEADLESS_MS=8000 RAE_FRAME_TIMING=1 RAE_GPU_PASS_TIMING=1 \
    RAE_WGPU_REPORT=1 perl -e 'alarm shift; exec @ARGV' 150 \
      "$root/compiler/bin/rae" run --project "$root/examples/138_procgen_showcase" \
      "$root/examples/138_procgen_showcase/Main.rae" > "$output/metrics-138.log" 2>&1
  env RAE_CAMERA_AT='1 0 0.24 -0.06 4.2 0 -18 2.4 0 0 0 0' \
    RAE_WALKER_HOUR=12 RAE_WALKER_DAY=298 RAE_DRS=off RAE_DEBUG_OVERLAY=0 \
    RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=12000 RAE_FRAME_TIMING=1 \
    RAE_GPU_PASS_TIMING=1 RAE_WGPU_REPORT=1 \
    perl -e 'alarm shift; exec @ARGV' 180 "$root/compiler/bin/rae" run \
      --project "$root/examples/143_walker_forest" \
      "$root/examples/143_walker_forest/Main.rae" > "$output/metrics-143.log" 2>&1
}

case "$mode" in
  captures) captureSet ;;
  verify) verifyStable ;;
  metrics) captureMetrics ;;
  all) captureSet; verifyStable; captureMetrics ;;
  *) printf '%s\n' "usage: $0 [captures|verify|metrics|all]" >&2; exit 2 ;;
esac

printf '%s\n' "landscape comparison output: $output"
