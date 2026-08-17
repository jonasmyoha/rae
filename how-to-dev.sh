#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVTOOLS_DIR="$ROOT_DIR/tools/devtools-web"

showHelp() {
  cat <<'EOF'
Rae monorepo development helper

Usage: ./how-to-dev.sh <command>

Commands:
  setup       Install Devtools dependencies and build the compiler
  build       Build the Rae compiler
  test        Run the compiler test suite
  dashboard   Start Devtools Web
  lint        Type-check Devtools Web
  help        Show this message
EOF
}

case "${1:-help}" in
  setup)
    "$ROOT_DIR/setup.sh"
    ;;
  build)
    make -C "$ROOT_DIR/compiler" build
    ;;
  test)
    make -C "$ROOT_DIR/compiler" test
    ;;
  dashboard)
    if [ ! -d "$DEVTOOLS_DIR/node_modules" ]; then
      (cd "$DEVTOOLS_DIR" && bun install --frozen-lockfile)
    fi
    make -C "$DEVTOOLS_DIR" dev
    ;;
  lint)
    (cd "$DEVTOOLS_DIR" && bun run lint)
    ;;
  help|--help|-h)
    showHelp
    ;;
  *)
    echo "Unknown command: $1" >&2
    showHelp >&2
    exit 1
    ;;
esac
