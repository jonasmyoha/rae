#!/bin/bash
# ui-model-gate.sh — one UI coordinate model, enforced (docs/ui-coordinate-and-responsive-
# layout.md: a design unit is a logical point and the layout fills the window). Fails when:
#   1. a program (examples/, packages/) calls Gpu2d.setDesignResolution itself, outside the
#      files in ui-model-allowlist.txt (each with its reason); lib/ui, lib/app3d and checks/
#      are not looked at (the lib sets the standard; a check sets a fixed headless size);
#   2. a .raescene declares its own unit or design size (a top-level "unitScale",
#      "designWidth", "designHeight" or "designSize"); a ScaleToFit subtree's own Rect is not
#      a declaration and is allowed;
#   3. a program's code names `unitScale` (the constant is lib/ui/DesignUnits').
# An allowlist line whose file no longer calls it fails as STALE. One pre-suite case of every
# full run; well under a second.
#
#   bash compiler/tools/ui-model-gate.sh
set -u
cd "$(dirname "$0")/../.." || exit 1
ALLOW="compiler/tools/ui-model-allowlist.txt"
FAILED=0

allowed=$(grep -v '^#' "$ALLOW" | awk 'NF {print $1}')
callers=$(git grep -l "setDesignResolution(" -- 'examples/*.rae' 'packages/*.rae' 2>/dev/null \
  | grep -v '^examples/legacy/' | while read -r f; do
      if grep -v '^\s*#' "$f" | grep -q "setDesignResolution("; then echo "$f"; fi
    done)
for f in $callers; do
  if ! printf '%s\n' "$allowed" | grep -qx "$f"; then
    echo "  $f calls Gpu2d.setDesignResolution itself: lay out in the window's points through app3d/UiShell (syncDesignResolution), or draw a fixed area as a stage (Gpu2dCanvas canvasPushStage)"
    FAILED=1
  fi
done
for f in $allowed; do
  if ! printf '%s\n' "$callers" | grep -qx "$f"; then
    echo "  STALE: $f is in $ALLOW but no longer calls setDesignResolution; remove its line"
    FAILED=1
  fi
done

declares=$(git grep -lE '^[[:space:]]{0,2}"(unitScale|designWidth|designHeight|designSize)"[[:space:]]*:' -- '*.raescene' 2>/dev/null \
  | grep -v '^examples/legacy/')
for f in $declares; do
  echo "  $f declares its own unit or design size: a scene is in points (convert it with tools/scale-ui-lengths.py)"
  FAILED=1
done

named=$(git grep -n "unitScale" -- 'examples/*.rae' 'packages/*.rae' 2>/dev/null | grep -v '^examples/legacy/')
if [ -n "$named" ]; then
  printf '%s\n' "$named" | sed 's/^/  names unitScale (the constant is lib\/ui\/DesignUnits'"'"'): /'
  FAILED=1
fi

if [ "$FAILED" -ne 0 ]; then
  echo "FAIL: ui-model-gate"
  exit 1
fi
echo "PASS: ui-model-gate (no own design resolution outside the $(printf '%s\n' "$allowed" | grep -c .) allowed files, no scene unit, no unitScale in programs)"
