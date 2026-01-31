#!/bin/bash
# Format all Rae source files in the project

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$ROOT_DIR/compiler/bin/rae"

if [ ! -f "$BIN" ]; then
    echo "Error: rae compiler not found at $BIN. Build it first."
    exit 1
fi

echo "Formatting all Rae source files..."

# Find and format examples and lib
find "$ROOT_DIR/examples" "$ROOT_DIR/lib" -name "*.rae" -type f -exec "$BIN" format -w {} \;

echo "Done."
