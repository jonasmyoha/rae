#!/usr/bin/env bash
# Snapshot a music-player screen through the GPU2D/SDL3 path.
#
# Usage:
#   examples/106_mobile_ui/snapshot.sh [output.bmp] [screen]
#
# `screen` is "home" (default), "album", "player", "library", "search",
# "profile", or "history". The output is BMP because the runtime screenshot helper
# writes SDL surfaces directly.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

SCREEN="${2:-home}"
OUT="${1:-examples/106_mobile_ui/temp/screenshots/${SCREEN}-gpu2d.bmp}"
DEVICE="${RAE_UI_DEVICE:-iphone-15-pro}"
FRAME="${RAE_UI_FRAME:-PhonePortrait}"
mkdir -p "$(dirname "$OUT")"

perl -e 'alarm shift; exec @ARGV' 30 \
  env RAE_UI_SCREEN="$SCREEN" \
      RAE_UI_DEVICE="$DEVICE" \
      RAE_UI_FRAME="$FRAME" \
      RAE_UI_NO_SPOTIFY=1 \
      RAE_AUTO_EXIT_SEC=1 \
      RAE_SDL_HEADLESS_MS=1000 \
      RAE_GPU2D_SCREENSHOT="$OUT" \
  compiler/bin/rae run --project examples/106_mobile_ui --target compiled examples/106_mobile_ui/main.rae

echo "Saved $OUT"
