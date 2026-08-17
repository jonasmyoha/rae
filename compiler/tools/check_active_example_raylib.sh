#!/bin/bash
# Reject Raylib imports outside the temporary, shrinking migration allowlist.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ALLOWLIST="$ROOT/compiler/tools/active_raylib_examples.txt"
ACTUAL=$(mktemp)
trap 'rm -f "$ACTUAL"' EXIT

find "$ROOT/examples" \
  -path "$ROOT/examples/legacy" -prune -o \
  -type f -name '*.rae' \
  -exec sh -c 'grep -IlE '\''^[[:space:]]*(open|import)[[:space:]]+"?raylib"?([[:space:]]|$)'\'' "$@" || true' sh {} + \
  | sed "s#^$ROOT/##" \
  | sort > "$ACTUAL"

if ! diff -u "$ALLOWLIST" "$ACTUAL"; then
  echo "Active Raylib imports changed. Migrate/remove imports, or update the shrinking allowlist intentionally." >&2
  exit 1
fi
