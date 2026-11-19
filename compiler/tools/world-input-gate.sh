#!/bin/bash
# world-input-gate.sh — a game reads the world's input, never the device (docs/ui-layers-design.md
# §7). Fails when a file in lib/app3d/ or packages/ reads the mouse or the keys from the device
# (Sdl3.isMouseButtonDown / isMouseButtonPressed / isKeyDown / isKeyPressed / isKeyReleased,
# Gpu2d.pointerPressed / pointerDown / pointerReleased) outside world-input-allowlist.txt (each
# line with its reason): such a read acts on a click meant for a button over the world. It reads
# ui/LayerCapture worldPointer / worldKeys or ui/WorldInput worldKeyDown / worldKeyPressed instead.
# lib/ui is where the device is read for the UI and the world, and is not looked at. An allowlist
# line whose file no longer reads the device fails as STALE. One pre-suite case of every full run.
#
#   bash compiler/tools/world-input-gate.sh
set -u
cd "$(dirname "$0")/../.." || exit 1
ALLOW="compiler/tools/world-input-allowlist.txt"
PATTERN='Sdl3\.(isMouseButtonDown|isMouseButtonPressed|isMouseButtonReleased|isKeyDown|isKeyPressed|isKeyReleased)\(|Gpu2d\.(pointerPressed|pointerDown|pointerReleased)\('
FAILED=0

allowed=$(grep -v '^#' "$ALLOW" | awk 'NF {print $1}')
readers=$(git grep -lE "$PATTERN" -- 'lib/app3d/*.rae' 'packages/*.rae' 2>/dev/null | while read -r f; do
  if grep -v '^\s*#' "$f" | grep -qE "$PATTERN"; then echo "$f"; fi
done)
for f in $readers; do
  if ! printf '%s\n' "$allowed" | grep -qx "$f"; then
    grep -nE "$PATTERN" "$f" | grep -v '^[0-9]*:\s*#' | head -3 | sed "s|^|  $f:|"
    echo "  $f reads the device: read the world's input (ui/LayerCapture worldPointer, ui/WorldInput worldKeyDown / worldKeyPressed), or add the file to $ALLOW with its reason"
    FAILED=1
  fi
done
for f in $allowed; do
  if ! printf '%s\n' "$readers" | grep -qx "$f"; then
    echo "  STALE: $f is in $ALLOW but no longer reads the device; remove its line"
    FAILED=1
  fi
done

if [ "$FAILED" -ne 0 ]; then
  echo "FAIL: world-input-gate"
  exit 1
fi
echo "PASS: world-input-gate (no device mouse or key read in lib/app3d or packages outside the $(printf '%s\n' "$allowed" | grep -c .) allowed files)"
