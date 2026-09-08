#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT
sed -n '/BEGIN PRESENT RECOVERY/,/END PRESENT RECOVERY/p' \
  runtime/runtime_gpu2d_frame.c > "$temporary/present_recovery.inc"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
  -I"${WGPU_NATIVE:-$HOME/.local/wgpu-native}/include" -I"$temporary" \
  tests/present_recovery.c -o "$temporary/test"
"$temporary/test"
