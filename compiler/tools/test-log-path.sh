#!/usr/bin/env bash
# The live test log of the checkout this runs in, which watch-tests.sh writes and holds its lock
# beside (AGENTS.md "Testing"):
#
#   RAE_TEST_LOG, when set;
#   /tmp/rae-test-live.log for the repository's main checkout (Rae devtools tails that one);
#   /tmp/rae-test-live-<folder>-<8 hex of its path>.log for a linked worktree
#   (`git worktree add`), so test runs in several worktrees run side by side and never empty
#   or wait for each other's log.
set -u
if [ -n "${RAE_TEST_LOG:-}" ]; then
  printf '%s\n' "$RAE_TEST_LOG"
  exit 0
fi
here="$(cd "$(dirname "$0")" && pwd)"
top="$(git -C "$here" rev-parse --show-toplevel 2>/dev/null)"
dir="$(git -C "$here" rev-parse --absolute-git-dir 2>/dev/null)"
common="$(git -C "$here" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"
if [ -z "$top" ] || [ "$dir" = "$common" ]; then
  printf '/tmp/rae-test-live.log\n'
  exit 0
fi
hash="$(printf '%s' "$top" | shasum | cut -c1-8)"
printf '/tmp/rae-test-live-%s-%s.log\n' "$(basename "$top")" "$hash"
