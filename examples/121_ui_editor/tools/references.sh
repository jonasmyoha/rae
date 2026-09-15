#!/usr/bin/env bash
# Regenerate the checked-in visual references for the editor's sample scenes
# (#1000): one headless screenshot per sample, converted to PNG under
# examples/121_ui_editor/references/. The example gate compares fresh
# screenshots against these with compiler/tools/assert_bmp_diff.py (tolerant:
# under 0.1% of pixels may differ). Run from anywhere; needs a built
# compiler/bin/rae.
#
#   make -C examples/121_ui_editor references
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$REPO_ROOT"
OUT_DIR="examples/121_ui_editor/references"
mkdir -p "$OUT_DIR"
TMP="$(mktemp -d)"
for SAMPLE in MainMenu Settings CardStrip Coverage; do
  RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/$SAMPLE.raescene" \
  RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1500 RAE_GPU2D_SCREENSHOT="$TMP/$SAMPLE.bmp" \
  perl -e 'alarm shift; exec @ARGV' 120 \
    compiler/bin/rae run --project examples/121_ui_editor examples/121_ui_editor/Main.rae \
    > "$TMP/$SAMPLE.log" 2>&1 || { echo "render of $SAMPLE failed:"; grep -v '^\[present\]' "$TMP/$SAMPLE.log" | tail -20; exit 1; }
  grep -qE "\[ui-editor\] mounted $SAMPLE: [1-9][0-9]* nodes, 0 diagnostics" "$TMP/$SAMPLE.log" \
    || { echo "$SAMPLE mounted with diagnostics:"; grep '\[ui-editor\]' "$TMP/$SAMPLE.log"; exit 1; }
  python3 compiler/tools/assert_bmp_diff.py --convert "$TMP/$SAMPLE.bmp" "$OUT_DIR/$SAMPLE.png"
done
rm -rf "$TMP"
echo "references written to $OUT_DIR"
