#!/bin/bash
# Simulate downloading a new hybrid VM chunk by building the example bundle
# and copying the VM artifacts into a timestamped directory.

set -euo pipefail

ENTRY_PATH=${1:-}
PROFILE=${2:-dev}
VERSION=${3:-version1}

if [ -z "$ENTRY_PATH" ]; then
  echo "error: simulate_download.sh expects the example entry path" >&2
  exit 1
fi

WORKSPACE_DIR=$(pwd)
EXAMPLE_DIR=$(dirname "$ENTRY_PATH")
ABS_EXAMPLE_DIR=$WORKSPACE_DIR/"$EXAMPLE_DIR"
VERSION_DIR="$ABS_EXAMPLE_DIR/scripts/downloaded/$VERSION"
if [ ! -d "$VERSION_DIR" ]; then
  echo "error: unknown version '$VERSION' (expected scripts/downloaded/versionN)" >&2
  exit 1
fi

OUTPUT_ROOT="$ABS_EXAMPLE_DIR/.simulated_downloads/$PROFILE/$VERSION"
mkdir -p "$OUTPUT_ROOT"
TEMP_DIR=$(mktemp -d "${OUTPUT_ROOT}/build-XXXXXX")
BUNDLE_DIR="$TEMP_DIR/bundle"
cleanup() {
  rm -rf "$TEMP_DIR"
}
trap cleanup EXIT

PATCH_SRC="$VERSION_DIR/${PROFILE}_patch.rae"
PATCH_DEST="$ABS_EXAMPLE_DIR/scripts/downloaded/${PROFILE}_patch.rae"
if [ ! -f "$PATCH_SRC" ]; then
  echo "error: missing patch file $PATCH_SRC" >&2
  exit 1
fi

cp "$PATCH_SRC" "$PATCH_DEST"

echo "[simulate] Building hybrid bundle ($PROFILE profile, $VERSION) for $ENTRY_PATH"
./compiler/bin/rae build --target hybrid --profile "$PROFILE" --out "$BUNDLE_DIR" "$ENTRY_PATH"
if [ ! -d "$BUNDLE_DIR/vm" ]; then
  echo "error: expected VM outputs in $BUNDLE_DIR/vm" >&2
  exit 1
fi
STAMP=$(date +%Y%m%d-%H%M%S)
DEST_DIR="$OUTPUT_ROOT/$STAMP"
mkdir -p "$DEST_DIR"
cp "$BUNDLE_DIR"/vm/*.vmchunk "$DEST_DIR"/
cp "$BUNDLE_DIR"/vm/*.manifest.json "$DEST_DIR"/

python3 - "$DEST_DIR" <<'PY'
import hashlib
import os
import sys
from pathlib import Path

dest = Path(sys.argv[1]).resolve()
print(f"[simulate] Copied VM artifacts to {dest}")
for path in sorted(dest.iterdir()):
    if path.is_file():
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        print(f"[simulate] {path.name}: {path.stat().st_size} bytes {digest[:16]}…")
PY

echo "[simulate] Hybrid host can now reload files from $DEST_DIR"
