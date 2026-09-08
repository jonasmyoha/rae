#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
testBinary=$(mktemp /tmp/raeReadbackTest.XXXXXX)
trap 'rm -f "$testBinary"' EXIT
perl -e 'alarm shift; exec @ARGV' 60 "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -g tests/webgpuReadback.c -o "$testBinary"
perl -e 'alarm shift; exec @ARGV' 30 "$testBinary"
echo "PASS: webgpuReadback [ASan/UBSan callback ABI]"
