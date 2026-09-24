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

# The marquee is transient chrome, so a dedicated headless hook holds it over
# the Start button for one reference frame. The ordinary MainMenu reference
# remains the idle editor.
env RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" \
RAE_UI_EDITOR_TEST_MARQUEES="preview:PlayButton" \
RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1500 RAE_GPU2D_SCREENSHOT="$TMP/Marquee.bmp" \
perl -e 'alarm shift; exec @ARGV' 120 \
  compiler/bin/rae run --project examples/121_ui_editor examples/121_ui_editor/Main.rae \
  > "$TMP/Marquee.log" 2>&1 \
  || { echo "marquee render failed:"; grep -v '^\[present\]' "$TMP/Marquee.log" | tail -20; exit 1; }
grep -qF "[ui-editor] marquee preview:PlayButton -> none" "$TMP/Marquee.log" \
  || { echo "marquee preview did not run:"; grep '\[ui-editor\]' "$TMP/Marquee.log"; exit 1; }
python3 compiler/tools/assert_bmp_diff.py --convert "$TMP/Marquee.bmp" "$OUT_DIR/Marquee.png"

# The hierarchy's multi-selection plate is another interaction-only frame.
# Keep its gesture identical to the example gate: two selected rows and the
# brighter active row, with a populated inspector beside them.
env RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" \
RAE_UI_EDITOR_TEST_TREE_CLICKS="Title cmd:Tagline cmd:LogoRing" \
RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1500 \
RAE_GPU2D_SCREENSHOT="$TMP/TreeSelection.bmp" \
perl -e 'alarm shift; exec @ARGV' 120 \
  compiler/bin/rae run --project examples/121_ui_editor examples/121_ui_editor/Main.rae \
  > "$TMP/TreeSelection.log" 2>&1 \
  || { echo "tree-selection render failed:"; grep -v '^\[present\]' "$TMP/TreeSelection.log" | tail -20; exit 1; }
grep -qF "Title,Tagline,LogoRing active LogoRing" "$TMP/TreeSelection.log" \
  || { echo "tree-selection gesture did not run:"; grep '\[ui-editor\]' "$TMP/TreeSelection.log"; exit 1; }
python3 compiler/tools/assert_bmp_diff.py --convert \
  "$TMP/TreeSelection.bmp" "$OUT_DIR/TreeSelection.png"

# The component list scrolls as a moving virtual list inside a fixed clip.
# Capture a deep offset so a travelling clip or stale row window goes blank.
env RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" \
RAE_UI_EDITOR_TEST_SELECT="PlayButton" \
RAE_UI_EDITOR_TEST_COMPONENT="scroll:-280" \
RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1500 \
RAE_GPU2D_SCREENSHOT="$TMP/ComponentScroll.bmp" \
perl -e 'alarm shift; exec @ARGV' 120 \
  compiler/bin/rae run --project examples/121_ui_editor examples/121_ui_editor/Main.rae \
  > "$TMP/ComponentScroll.log" 2>&1 \
  || { echo "component-scroll render failed:"; grep -v '^\[present\]' "$TMP/ComponentScroll.log" | tail -20; exit 1; }
grep -qF "[ui-editor] component scroll: -280" "$TMP/ComponentScroll.log" \
  || { echo "component scroll did not run:"; grep '\[ui-editor\]' "$TMP/ComponentScroll.log"; exit 1; }
python3 compiler/tools/assert_bmp_diff.py --convert \
  "$TMP/ComponentScroll.bmp" "$OUT_DIR/ComponentScroll.png"

rm -rf "$TMP"
echo "references written to $OUT_DIR"
