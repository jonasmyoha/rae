#!/usr/bin/env bash
# checks/references.sh — write the reference pictures of the checks that are pictures (a
# reference.png in their folder; checks/run.sh compares each run's screenshot with it). Each is
# rendered headlessly for 10 frames and its screenshot converted to PNG. Look at a picture before
# committing it: the reference is what the next runs are held to.
#
#   bash checks/references.sh uiTimeline uiFlameHistogram uiLineTable   # these
#   bash checks/references.sh                                            # every check with a reference
set -euo pipefail
cd "$(dirname "$0")/.."
RAE="${RAE_BIN:-compiler/bin/rae}"
names=("$@")
if [ ${#names[@]} -eq 0 ]; then
  for dir in checks/*/; do
    [ -f "$dir/reference.png" ] && names+=("$(basename "$dir")")
  done
fi
TMP="$(mktemp -d -t rae_references_XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
for name in "${names[@]}"; do
  dir="checks/$name"
  env RAE_HEADLESS_FRAMES=10 RAE_GPU2D_SCREENSHOT="$TMP/$name.bmp" \
    perl -e 'alarm shift; exec @ARGV' 180 "$RAE" run --target compiled --project "$dir" "$dir/Main.rae" \
    > "$TMP/$name.log" 2>&1 || { echo "render of $name failed:"; grep -v '^\[present\]' "$TMP/$name.log" | tail -20; exit 1; }
  python3 compiler/tools/assert_bmp_diff.py --convert "$TMP/$name.bmp" "$dir/reference.png"
  echo "wrote $dir/reference.png"
done
