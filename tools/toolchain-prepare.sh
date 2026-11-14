#!/bin/sh
# Make one Rae toolchain ready in the store (docs/toolchain-and-packages-decision.md):
#
#   tools/toolchain-prepare.sh <home> <source> [<version> or <version>-<commit>]
#
# <home>/repo is one clone, fetched from <source>. With a name, <home>/toolchains/<name>
# becomes a sparse checkout of that version's tag, or that commit, with its compiler built
# and a .ready marker. A project's ./rae runs this from the Rae repo itself, and so does
# `rae toolchain`. Needs git (with git-lfs for a version whose images and fonts are in Git LFS),
# make and a C compiler. A failed build leaves nothing
# behind but its log.
#
#   tools/toolchain-prepare.sh --deps <home> <name> [box3d sqlite wamr ios android]
#
# The shared builds of a toolchain, in <home>/deps/<dep>/<pin>. The pin is a hash of the files
# that decide the build, read from that toolchain's own commit, so two versions with another
# Box3D use two folders and two versions with the same one share a folder. <toolchain>/.deps
# holds the variables that point its compiler and build scripts there (./rae reads it), and
# tools/ios/frameworks and tools/android/deps are links there. With no dep this says what is
# built; with deps it builds them, which only `rae toolchain deps` asks for. Those builds need
# more than a C compiler (cmake, and Xcode or the Android NDK).
set -eu
mode=prepare
if [ "${1:-}" = --deps ]; then
  mode=deps
  shift
fi
home=$1

# Take a lock folder, waiting a bounded time: several builds may start together. Returns 1
# when the marker it was waiting for appears, as the holder finished the work.
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
    if [ -n "$4" ] && [ -f "$4" ]; then return 1; fi
  done
  lock=$1
}
drop_lock() {
  [ -z "$lock" ] || rmdir "$lock" 2>/dev/null || true
  lock=
}
trap drop_lock EXIT
trap 'drop_lock; exit 130' INT TERM

# The files that decide a shared build, the first being what must exist in this version
dep_inputs() {
  case $1 in
    box3d) echo "tools/box3d/build.sh tools/box3d" ;;
    sqlite) echo "tools/sqlite/build.sh tools/sqlite" ;;
    wamr) echo "tools/wamr/build.sh tools/wamr compiler/runtime/runtime_wasm_embed.h" ;;
    ios) echo "tools/ios/build-sdl3-ios.sh tools/ios/build-wgpu-ios.sh" ;;
    android) echo "tools/android/build-sdl3-android.sh tools/android/build-wgpu-android.sh tools/android/env.sh" ;;
    *) return 1 ;;
  esac
}

# The folder of a shared build for the toolchain in $dir; empty when this version has none
dep_folder() {
  inputs=$(dep_inputs "$1")
  git -C "$dir" rev-parse -q --verify "HEAD:${inputs%% *}" >/dev/null || return 0
  pin=$(for input in $inputs; do
    git -C "$dir" rev-parse -q --verify "HEAD:$input" || true
  done | git hash-object --stdin | cut -c1-12)
  echo "$home/deps/$1/$pin"
}

# <toolchain>/.deps and the links. The caller's own value of a variable wins.
write_deps() {
  : > "$dir/.deps.tmp"
  for dep in box3d sqlite wamr; do
    folder=$(dep_folder "$dep")
    [ -n "$folder" ] || continue
    mkdir -p "$folder"
    variable=RAE_$(echo "$dep" | tr '[:lower:]' '[:upper:]')
    echo "export ${variable}_CACHE=\"\${${variable}_CACHE:-$folder}\"" >> "$dir/.deps.tmp"
    echo "export $variable=\"\${$variable:-$folder/install}\"" >> "$dir/.deps.tmp"
  done
  for dep in ios android; do
    folder=$(dep_folder "$dep")
    [ -n "$folder" ] || continue
    mkdir -p "$folder"
    if [ "$dep" = ios ]; then link=$dir/tools/ios/frameworks; else link=$dir/tools/android/deps; fi
    ln -sfn "$folder" "$link"
    echo "# $dep $folder" >> "$dir/.deps.tmp"
  done
  mv "$dir/.deps.tmp" "$dir/.deps"
}

build_dep() {
  case $1 in
    box3d | sqlite | wamr) bash "$dir/tools/$1/build.sh" ;;
    ios) sh "$dir/tools/ios/build-sdl3-ios.sh" && sh "$dir/tools/ios/build-wgpu-ios.sh" ;;
    android) sh "$dir/tools/android/build-sdl3-android.sh" && sh "$dir/tools/android/build-wgpu-android.sh" ;;
  esac
}

if [ "$mode" = deps ]; then
  name=$2
  shift 2
  dir=$home/toolchains/$name
  write_deps
  if [ $# -eq 0 ]; then
    for dep in box3d sqlite wamr ios android; do
      folder=$(dep_folder "$dep")
      if [ -z "$folder" ]; then echo "$dep: not in this version"
      elif [ -f "$folder/.ready" ]; then echo "$dep: built, $folder"
      else echo "$dep: not built: ./rae toolchain deps $dep"; fi
    done
    exit 0
  fi
  . "$dir/.deps"
  for dep in "$@"; do
    if ! dep_inputs "$dep" >/dev/null; then
      echo "rae: no shared build is called $dep: box3d, sqlite, wamr, ios or android" >&2
      exit 1
    fi
    folder=$(dep_folder "$dep")
    if [ -z "$folder" ]; then
      echo "rae: Rae $name has no $dep to build" >&2
      exit 1
    fi
    [ ! -f "$folder/.ready" ] || continue
    take_lock "$folder.lock" 720 5 "$folder/.ready" || continue
    if [ ! -f "$folder/.ready" ]; then
      echo "rae: building $dep for Rae $name in $folder" >&2
      if ! build_dep "$dep"; then
        echo "rae: the build of $dep failed" >&2
        exit 1
      fi
      touch "$folder/.ready"
    fi
    drop_lock
  done
  exit 0
fi

source=$2
name=${3:-}
repo=$home/repo
mkdir -p "$home/toolchains"

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
take_lock "$dir.lock" 240 5 "$dir/.ready" || exit 0
[ -f "$dir/.ready" ] && exit 0

log=$home/toolchains/$name.log
echo "rae: preparing Rae $name in $dir (the first time takes a minute)" >&2
git -C "$repo" worktree remove --force "$dir" >/dev/null 2>&1 || rm -rf "$dir"
git -C "$repo" worktree prune
git -C "$repo" worktree add --quiet --no-checkout --detach "$dir" "$found"
# The UI editor is a tool a project runs (its headless scene check), though it lives in examples
git -C "$dir" sparse-checkout set --cone compiler lib tools third_party examples/121_ui_editor
# The binary files (fonts, images) of a version that keeps them in Git LFS are pulled after the
# checkout, for the sparse folders only; without git-lfs they would be pointer files, and every
# UI program would fail on its font, so that stops here
uses_lfs=
if git -C "$repo" show "$found:.gitattributes" 2>/dev/null | grep -q 'filter=lfs'; then
  uses_lfs=1
fi
if [ -n "$uses_lfs" ] && ! command -v git-lfs >/dev/null 2>&1; then
  git -C "$repo" worktree remove --force "$dir" >/dev/null 2>&1 || rm -rf "$dir"
  git -C "$repo" worktree prune
  echo "rae: Rae $name keeps its fonts and images in Git LFS, and git-lfs is not installed (brew install git-lfs, or your system's git-lfs package)" >&2
  exit 1
fi
GIT_LFS_SKIP_SMUDGE=1 git -C "$dir" checkout --quiet --detach "$found"
if [ -n "$uses_lfs" ]; then
  if ! git -C "$dir" lfs pull --include "compiler,lib,tools,third_party,examples/121_ui_editor" >"$home/toolchains/$name.lfs.log" 2>&1; then
    tail -5 "$home/toolchains/$name.lfs.log" >&2
    git -C "$repo" worktree remove --force "$dir" >/dev/null 2>&1 || rm -rf "$dir"
    git -C "$repo" worktree prune
    echo "rae: could not fetch the Git LFS files of Rae $name from $source" >&2
    exit 1
  fi
fi
if ! make -j8 -C "$dir/compiler" build >"$log" 2>&1 || [ ! -x "$dir/compiler/bin/rae" ]; then
  tail -20 "$log" >&2
  git -C "$repo" worktree remove --force "$dir" >/dev/null 2>&1 || rm -rf "$dir"
  git -C "$repo" worktree prune
  echo "rae: the compiler of Rae $name does not build (log: $log)" >&2
  exit 1
fi
write_deps
touch "$dir/.ready"
