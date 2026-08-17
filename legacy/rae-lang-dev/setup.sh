#!/usr/bin/env bash
set -e

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

cd "$ROOT_DIR"

if [ ! -d rae ]; then
  git clone git@github.com:jonasmyoha/rae.git
fi

if [ ! -d rae-devtools-web ]; then
  git clone git@github.com:jonasmyoha/rae-devtools-web.git
fi

echo "Rae workspace ready:"
echo "  ./rae"
echo "  ./rae-devtools-web"
