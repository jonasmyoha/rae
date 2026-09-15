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
# Coverage animates (#1005), so its reference is frame 12 at a fixed 50 ms step
# (deterministic); the static samples take a wall-clock budget.
# split/Page imports by package path from the samples root (#1008), so the root
# is passed explicitly for every sample (the gate does the same).
for SAMPLE in MainMenu Settings CardStrip Coverage SelfContained split/Page; do
  NAME="$(basename "$SAMPLE")"
  BUDGET="RAE_SDL_HEADLESS_MS=1500"
  if [ "$SAMPLE" = "Coverage" ]; then BUDGET="RAE_HEADLESS_FRAMES=12 RAE_FIXED_DT=0.05"; fi
  env RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/$SAMPLE.raescene" \
  RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" \
  RAE_UI_HEADLESS=1 $BUDGET RAE_GPU2D_SCREENSHOT="$TMP/$NAME.bmp" \
  perl -e 'alarm shift; exec @ARGV' 120 \
    compiler/bin/rae run --project examples/121_ui_editor examples/121_ui_editor/Main.rae \
    > "$TMP/$NAME.log" 2>&1 || { echo "render of $SAMPLE failed:"; grep -v '^\[present\]' "$TMP/$NAME.log" | tail -20; exit 1; }
  grep -qE "\[ui-editor\] mounted $NAME: [1-9][0-9]* nodes, 0 diagnostics" "$TMP/$NAME.log" \
    || { echo "$SAMPLE mounted with diagnostics:"; grep '\[ui-editor\]' "$TMP/$NAME.log"; exit 1; }
  python3 compiler/tools/assert_bmp_diff.py --convert "$TMP/$NAME.bmp" "$OUT_DIR/$NAME.png"
done
rm -rf "$TMP"
echo "references written to $OUT_DIR"
