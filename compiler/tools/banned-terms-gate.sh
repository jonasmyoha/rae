#!/usr/bin/env bash
# banned-terms-gate.sh (#1016) — fail if a term that must never appear in this
# repository is in a tracked text file or in an unpushed commit message.
#
# The term list lives OUTSIDE the repository, on purpose: committing it would
# put the terms in the repo. One term per line, matched case-insensitively as
# a fixed string; blank lines and lines starting with `#` are ignored.
#
#   $RAE_BANNED_TERMS_FILE   the list (default ~/.config/rae/banned-terms.txt)
#
# No list file -> "SKIP" and exit 0, so CI and other machines are unaffected.
# Every hit is printed as `path:line` (tracked files) or `<sha> message`
# (commit messages) and the gate exits 1. Runs as one extra "case" of the full
# suite next to format-check-tree, and from the opt-in git hooks in
# tools/git-hooks/ (`make install-hooks`).
#
#   bash compiler/tools/banned-terms-gate.sh            # tree + unpushed messages
#   bash compiler/tools/banned-terms-gate.sh --staged   # the index only (pre-commit)
#   bash compiler/tools/banned-terms-gate.sh --message <file>   # one message (commit-msg)
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1     # -> repo root

LIST="${RAE_BANNED_TERMS_FILE:-$HOME/.config/rae/banned-terms.txt}"
if [ ! -f "$LIST" ]; then
  echo "SKIP: banned-terms-gate (no term list at $LIST; set RAE_BANNED_TERMS_FILE)"
  exit 0
fi
# The effective pattern file: comments and blanks dropped. Kept in a temp
# file so the terms are never on a command line (visible in `ps`).
PATTERNS="$(mktemp -t rae_banned_XXXXXX)"
trap 'rm -f "$PATTERNS"' EXIT
grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$LIST" > "$PATTERNS"
if [ ! -s "$PATTERNS" ]; then
  echo "SKIP: banned-terms-gate (the term list at $LIST is empty)"
  exit 0
fi

MODE="${1:-tree}"
HITS=0
SCOPE="the tree or unpushed messages"

# Print the hits of one source under a heading and add them to the total. The
# hits arrive as a string, not a pipe, so the count lands in THIS shell.
report() {  # $1 = where, $2 = hit lines (may be empty)
  local where="$1" lines="$2" n=0
  [ -z "$lines" ] && return 0
  echo "  $where:"
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    echo "    $line"
    n=$((n + 1))
  done <<< "$lines"
  HITS=$((HITS + n))
}

case "$MODE" in
  --message)
    # commit-msg hook: the message file is $2.
    SCOPE="the commit message"
    report "commit message" "$(grep -n -i -F -f "$PATTERNS" -- "$2" 2>/dev/null)" ;;
  --staged)
    # pre-commit hook: only what is about to be committed — the added lines of
    # the staged diff (a binary shows no `+` lines), reported by path.
    SCOPE="the staged changes"
    report "staged changes" "$(git diff --cached -U0 --no-color 2>/dev/null \
      | awk '/^\+\+\+ b\//{f=substr($0,7)} /^\+/ && !/^\+\+\+/{print f ": " substr($0,2)}' \
      | grep -i -F -f "$PATTERNS")" ;;
  *)
    # The whole tracked tree (text files only), then the messages of every
    # commit not yet on origin/main (the ones a force-push would have to erase).
    report "tracked files" "$(git grep -I -i -n -F -f "$PATTERNS" -- . 2>/dev/null)"
    if git rev-parse --verify -q origin/main >/dev/null 2>&1; then
      report "unpushed commit messages (origin/main..HEAD)" \
        "$(git log --format='%h %s%n%b' origin/main..HEAD 2>/dev/null | grep -i -F -f "$PATTERNS")"
    fi ;;
esac

if [ "$HITS" -gt 0 ]; then
  echo "FAIL: banned-terms-gate — $HITS hit(s) of a term from $LIST (see above)"
  exit 1
fi
echo "PASS: banned-terms-gate (no term from $LIST in $SCOPE)"
exit 0
