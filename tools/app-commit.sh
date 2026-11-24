#!/bin/sh
# What an app on a phone says it was built from (tools/ios/gen-ios.sh,
# tools/android/gen-android.sh: the log's first lines).
#
#   sh tools/app-commit.sh rae          this Rae: its version, and in a git
#                                       checkout its commit
#   sh tools/app-commit.sh app <folder> the app's commit: RAE_APP_COMMIT when the
#                                       app's own build sets it, else that of the
#                                       git repo the project is in
#
# A "+" after a commit: tracked files were changed. "unknown" without git. The
# answer holds only letters, digits, spaces and ._+()/-, so it can go into a plist.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

commit_of() {
  found=$(git -C "$1" rev-parse --short=12 HEAD 2>/dev/null) || return 1
  if [ -n "$(git -C "$1" status --porcelain --untracked-files=no 2>/dev/null | head -1)" ]; then
    found="$found+"
  fi
  printf '%s' "$found"
}

case "${1:-}" in
  rae)
    answer=$(head -1 "$here/../compiler/VERSION" 2>/dev/null || echo unknown)
    # Only this checkout's own git: a Rae in the store is no repo, and what lies
    # above it is not Rae
    if [ -e "$here/../.git" ] && commit=$(commit_of "$here/.."); then answer="$answer ($commit)"; fi
    ;;
  app)
    answer=${RAE_APP_COMMIT:-}
    [ -n "$answer" ] || answer=$(commit_of "${2:?usage: app-commit.sh app <folder>}") || answer=unknown
    ;;
  *) echo "usage: app-commit.sh rae | app <folder>" >&2; exit 1 ;;
esac
printf '%s\n' "$answer" | tr -cd 'A-Za-z0-9 ._+()/\n-'
