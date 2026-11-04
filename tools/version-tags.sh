#!/usr/bin/env bash
# A tag on every version bump (docs/toolchain-and-packages-decision.md section 5):
# v<version> names the commit that set compiler/VERSION to <version>, so that a
# project can pin a version and mean one fixed set of files.
#
#   tools/version-tags.sh --head             tag HEAD if it changes the version
#                                            (the post-commit and post-merge hooks)
#   tools/version-tags.sh --rewritten        move tags after a rebase or an amend:
#                                            "old new" lines on stdin (post-rewrite)
#   tools/version-tags.sh --history [--apply] [ref]
#                                            add the tags past bumps never got (ref:
#                                            main). Without --apply it only lists them.
#
# A hook never fails: a commit must not be lost over a tag. A tag that is already
# on a remote branch never moves, because a version must keep naming one thing.
# Nothing here pushes. `git push --follow-tags` sends the tags.
set -u

VERSION_FILE="compiler/VERSION"

version_at() {
  local version
  version="$(git show "$1:$VERSION_FILE" 2>/dev/null | tr -d ' \t\r\n')"
  case "$version" in
    *[!0-9.]*|"") return 1 ;;
  esac
  if printf '%s' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    printf '%s' "$version"
    return 0
  fi
  return 1
}

# Whether $1 sets the version: its version differs from every parent's
sets_version() {
  local commit="$1" version="$2" parent parent_version
  for parent in $(git rev-parse "$commit^@" 2>/dev/null); do
    parent_version="$(version_at "$parent" || true)"
    [ "$parent_version" = "$version" ] && return 1
  done
  return 0
}

is_published() {
  [ -n "$(git for-each-ref --contains "$1" --count=1 refs/remotes 2>/dev/null)" ]
}

make_tag() {
  # -f only ever moves a tag that is not published (the callers check)
  git -c tag.gpgsign=false tag -f -a "$1" -m "Rae ${1#v}" "$2" >/dev/null 2>&1
}

tag_head() {
  local version tag target head
  head="$(git rev-parse HEAD)" || return 0
  version="$(version_at HEAD)" || return 0
  sets_version HEAD "$version" || return 0
  tag="v$version"
  target="$(git rev-parse -q --verify "refs/tags/$tag^{commit}" 2>/dev/null || true)"
  if [ -z "$target" ]; then
    make_tag "$tag" "$head" && echo "version-tags: tagged $tag"
    return 0
  fi
  [ "$target" = "$head" ] && return 0
  if is_published "$target"; then
    echo "version-tags: warning: $tag already names $(git rev-parse --short "$target"), which is on a remote. It stays there, so this commit has no tag: bump $VERSION_FILE again." >&2
    return 0
  fi
  make_tag "$tag" "$head" && echo "version-tags: moved $tag to this commit (it was on an unpushed $(git rev-parse --short "$target"))"
}

follow_rewrites() {
  local old new rest tag
  while read -r old new rest; do
    [ -n "$old" ] && [ -n "$new" ] || continue
    for tag in $(git tag --points-at "$old" --list 'v[0-9]*.[0-9]*.[0-9]*'); do
      [ "$(version_at "$new" || true)" = "${tag#v}" ] || continue
      is_published "$old" && continue
      make_tag "$tag" "$new" && echo "version-tags: moved $tag with the rewrite"
    done
  done
}

# 0.3.0 to 0.10.0 of September 2026 were abandoned: ad546bbc2 reset 0.10.0 to 0.1.7 and they
# were never released. Those numbers name the later versions, so no tag for the old commits.
abandoned_version() {  # version, commit
  case "$1" in
    0.[3-9].* | 0.10.*) git merge-base --is-ancestor "$2" ad546bbc2 2>/dev/null ;;
    *) return 1 ;;
  esac
}

tag_history() {
  local apply=0 ref="main" argument listed commit version made=0 kept=0 tag date
  for argument in "$@"; do
    case "$argument" in
      --apply) apply=1 ;;
      *) ref="$argument" ;;
    esac
  done
  listed="$(mktemp)"
  # Oldest first, so a later bump of a reused version replaces the earlier one
  git rev-list --full-history --topo-order --reverse "$ref" -- "$VERSION_FILE" | while read -r commit; do
    version="$(version_at "$commit")" || continue
    sets_version "$commit" "$version" || continue
    abandoned_version "$version" "$commit" && continue
    echo "$version $commit"
  done > "$listed"
  local bumps reused
  bumps="$(wc -l < "$listed" | tr -d ' ')"
  reused="$(awk '{seen[$1]++} END {count=0; for (v in seen) if (seen[v]>1) count++; print count}' "$listed")"
  # The latest commit of each version, in the order they first appear
  while read -r version commit; do
    tag="v$version"
    if git rev-parse -q --verify "refs/tags/$tag" >/dev/null 2>&1; then
      kept=$((kept + 1))
      continue
    fi
    made=$((made + 1))
    if [ "$apply" = 1 ]; then
      date="$(git log -1 --format=%cI "$commit")"
      GIT_COMMITTER_DATE="$date" make_tag "$tag" "$commit" || { echo "version-tags: could not tag $tag" >&2; rm -f "$listed"; return 1; }
    else
      echo "$tag $(git rev-parse --short "$commit")"
    fi
  done < <(awk '{latest[$1]=$2; if (!($1 in order)) {order[$1]=NR}} END {for (v in latest) print order[v], v, latest[v]}' "$listed" | sort -n | cut -d' ' -f2-)
  rm -f "$listed"
  if [ "$apply" = 1 ]; then
    echo "version-tags: $bumps bumps, $reused versions used more than once (the latest commit has the tag), $made tags added, $kept already there"
  else
    echo "version-tags: $bumps bumps, $reused versions used more than once (the latest commit has the tag), $made tags to add, $kept already there. Run again with --apply."
  fi
}

case "${1:-}" in
  --head) tag_head; exit 0 ;;
  --rewritten) follow_rewrites; exit 0 ;;
  --history) shift; tag_history "$@" ;;
  *) echo "usage: tools/version-tags.sh --head | --rewritten | --history [--apply] [ref]" >&2; exit 2 ;;
esac
