#!/bin/bash
# example-copy-gate.sh — fail when an example file is a copy of another
# example's file or of a lib/ file, unless the pair is in
# example-copy-allowlist.txt with its reason (AGENTS.md "Example families").
#
# Why: 137 and 143 began as copies of 136, and every later 136 improvement
# (the sea, the splashes, the terrain look, the rocks) landed in 136's own
# files, so the copies silently fell behind. Code an example family shares
# belongs in lib/, used by every member. The allowlist starts with the copies
# that existed when the gate was added and may only shrink: a pair that is no
# longer a copy fails as STALE until its line is removed.
#
# Two files are a copy pair at >= 40 shared meaningful lines that are also at
# least half of the smaller file (example_copy_check.py). Runs in well under
# a second; one pre-suite case of every full run.
#
#   bash compiler/tools/example-copy-gate.sh          # check
#   python3 compiler/tools/example_copy_check.py . compiler/tools/example-copy-allowlist.txt --list
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP: example-copy-gate (no python3)"
  exit 0
fi
if python3 "$HERE/example_copy_check.py" "$ROOT" "$HERE/example-copy-allowlist.txt" > /tmp/rae-example-copy-gate.$$ 2>&1; then
  echo "PASS: example-copy-gate ($(tail -1 /tmp/rae-example-copy-gate.$$))"
  rm -f /tmp/rae-example-copy-gate.$$
  exit 0
fi
cat /tmp/rae-example-copy-gate.$$
rm -f /tmp/rae-example-copy-gate.$$
echo "FAIL: example-copy-gate"
exit 1
