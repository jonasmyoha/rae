#!/usr/bin/env bash
# Regenerate the low-level Rae bindings to upstream Box3D's C API (lib/box3d,
# the C-library physics track, docs/physics-two-implementations.md §4) from the
# headers tools/box3d/build.sh installed. Output is deterministic and checked
# in; run this only when the pin (tools/box3d/pin) or the generator changes.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RAE="$REPO_ROOT/compiler/bin/rae"
BOX3D="${RAE_BOX3D:-$HOME/.cache/rae/box3d/install}"
INC="$BOX3D/include/box3d"

if [ ! -x "$RAE" ]; then echo "build the compiler first: (cd compiler && make)"; exit 1; fi
if [ ! -f "$INC/box3d.h" ]; then echo "box3d.h not found under $INC (run tools/box3d/build.sh)"; exit 1; fi

# Headers in dependency order: a struct is defined before the structs and
# functions that hold it by value.
"$RAE" bindgen \
  "$INC/base.h" "$INC/constants.h" "$INC/math_functions.h" "$INC/id.h" \
  "$INC/collision.h" "$INC/types.h" "$INC/box3d.h" \
  --out-dir "$REPO_ROOT/lib/box3d" \
  --module box3d \
  --import-prefix box3d \
  --cheader "box3d/box3d.h" \
  --api-macro B3_API \
  --inline-macro B3_INLINE --inline-macro B3_FORCE_INLINE --inline-macro B3_ID_INLINE \
  --define-prefix B3_ \
  --module-comment "Box3D $(cat "$BOX3D/COMMIT") (upstream's C library, tools/box3d) low-level bindings."

echo "Regenerated lib/box3d/Box3d*.rae"
