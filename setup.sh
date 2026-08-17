#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVTOOLS_DIR="$ROOT_DIR/tools/devtools-web"

echo "Setting up the Rae monorepo..."

if command -v bun >/dev/null 2>&1; then
  echo "Installing Devtools Web dependencies..."
  (cd "$DEVTOOLS_DIR" && bun install --frozen-lockfile)
else
  echo "warning: Bun is not installed; skipping Devtools Web dependencies" >&2
  echo "         install Bun, then run: make devtools-install" >&2
fi

echo "Building the Rae compiler..."
make -C "$ROOT_DIR/compiler" build

echo "Rae monorepo setup complete."
echo "  Compiler:     ./compiler/bin/rae"
echo "  Devtools Web: make dev"
