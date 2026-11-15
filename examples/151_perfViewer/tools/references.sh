#!/usr/bin/env bash
# examples/151_perfViewer/tools/references.sh — write the viewer's reference pictures: each view of
# the walker sample capture (assets/walker.raeperf), and the Compare view of the A/B sample
# (assets/ab-walker.raeperf, its baseline against ssao), rendered headlessly for 10 frames, as
# references/<view>.png. compiler/tools/run_examples.sh compares each run's pictures with them. Look
# at every picture before committing it.
#
#   bash examples/151_perfViewer/tools/references.sh            # every view
#   bash examples/151_perfViewer/tools/references.sh timeline   # one
set -euo pipefail
cd "$(dirname "$0")/../../.."
RAE="${RAE_BIN:-compiler/bin/rae}"
views=("$@")
if [ ${#views[@]} -eq 0 ]; then
  views=(frames timeline flame threads gpu audio memory systems compare)
fi
TMP="$(mktemp -d -t rae_perfviewer_XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
for view in "${views[@]}"; do
  capture="examples/151_perfViewer/assets/walker.raeperf"
  if [ "$view" = compare ]; then
    capture="examples/151_perfViewer/assets/ab-walker.raeperf"
  fi
  RAE_PERF_OPEN="$capture" RAE_PERF_VIEW="$view" RAE_HEADLESS_FRAMES=10 RAE_GPU2D_SCREENSHOT="$TMP/$view.bmp" \
    perl -e 'alarm shift; exec @ARGV' 180 "$RAE" run examples/151_perfViewer/Main.rae > "$TMP/$view.log" 2>&1 \
    || { echo "render of $view failed:"; tail -20 "$TMP/$view.log"; exit 1; }
  python3 compiler/tools/assert_bmp_diff.py --convert "$TMP/$view.bmp" "examples/151_perfViewer/references/$view.png"
done
