#!/bin/sh
# Make one Rae toolchain ready in the store (docs/toolchain-and-packages-decision.md):
#
#   tools/toolchain-prepare.sh <home> <source> [<version> or <version>-<commit>]
#
# <home>/repo is one clone, fetched from <source>. With a name, <home>/toolchains/<name>
# becomes a sparse checkout of that version's tag, or that commit, with its compiler built
# and a .ready marker. A project's ./rae runs this from the Rae repo itself, and so does
# `rae toolchain`. Needs only git, make and a C compiler. A failed build leaves nothing
# behind but its log.
set -eu
home=$1
source=$2
name=${3:-}
repo=$home/repo
mkdir -p "$home/toolchains"

# Take a lock folder, waiting a bounded time: several builds may start together
lock=
take_lock() {  # folder, tries, seconds between tries, ready marker to stop waiting at
  tries=0
  until mkdir "$1" 2>/dev/null; do
    tries=$((tries + 1))
    if [ "$tries" -gt "$2" ]; then
      echo "rae: $1 is still held; remove it if nothing is running" >&2
      exit 1
    fi
    sleep "$3"
    if [ -n "$4" ] && [ -f "$4" ]; then exit 0; fi
  done
  lock=$1
}
drop_lock() {
  [ -z "$lock" ] || rmdir "$lock" 2>/dev/null || true
  lock=
}
trap drop_lock EXIT
trap 'drop_lock; exit 130' INT TERM

take_lock "$home/repo.lock" 300 1 ""
if [ ! -d "$repo/.git" ]; then
  git clone --quiet --no-checkout "$source" "$repo"
fi
# Offline, what the clone holds may be enough
git -C "$repo" fetch --quiet --tags --force "$source" '+refs/heads/*:refs/remotes/source/*' || \
  echo "rae: could not fetch from $source; using what $repo has" >&2
drop_lock
[ -n "$name" ] || exit 0

dir=$home/toolchains/$name
[ -f "$dir/.ready" ] && exit 0
version=${name%%-*}
commit=${name#"$version"}
commit=${commit#-}
if [ -n "$commit" ]; then
  ref=$commit
else
  ref=refs/tags/v$version
fi
if ! found=$(git -C "$repo" rev-parse -q --verify "$ref^{commit}"); then
  if [ -n "$commit" ]; then
    echo "rae: commit $commit is not in $source. It may never have been pushed, or history was rewritten. Pin the version instead: ./rae toolchain pin $version" >&2
  else
    echo "rae: version $version has no tag v$version in $source" >&2
  fi
  exit 1
fi
at=$(git -C "$repo" show "$found:compiler/VERSION" | tr -d ' \t\r\n')
if [ "$at" != "$version" ]; then
  echo "rae: $name is wrong: the compiler/VERSION of that commit is $at" >&2
  exit 1
fi

# One preparation of a toolchain at a time, the others waiting for its marker
take_lock "$dir.lock" 240 5 "$dir/.ready"
[ -f "$dir/.ready" ] && exit 0

log=$home/toolchains/$name.log
echo "rae: preparing Rae $name in $dir (the first time takes a minute)" >&2
git -C "$repo" worktree remove --force "$dir" >/dev/null 2>&1 || rm -rf "$dir"
git -C "$repo" worktree prune
git -C "$repo" worktree add --quiet --no-checkout --detach "$dir" "$found"
git -C "$dir" sparse-checkout set --cone compiler lib tools third_party
git -C "$dir" checkout --quiet --detach "$found"
if ! make -j8 -C "$dir/compiler" build >"$log" 2>&1 || [ ! -x "$dir/compiler/bin/rae" ]; then
  tail -20 "$log" >&2
  git -C "$repo" worktree remove --force "$dir" >/dev/null 2>&1 || rm -rf "$dir"
  git -C "$repo" worktree prune
  echo "rae: the compiler of Rae $name does not build (log: $log)" >&2
  exit 1
fi
touch "$dir/.ready"
