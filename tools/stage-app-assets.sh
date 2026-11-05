#!/bin/sh
# Stage the files a compiled Rae app reads at run time into one folder, each
# at the relative path the app names it by, for a device bundle (tools/ios's
# .app, tools/android's APK). A device app chdirs into its bundle at launch,
# so the same relative reads resolve there as at the repo root.
#
#   sh tools/stage-app-assets.sh <project-dir> <the app's emitted C> <stage dir>
#
# Rae examples read assets by two conventions: repo-root-relative (e.g. 136's
# "lib/assets/walker/...", "lib/data/...") and {dir}/CWD-
# relative ("assets/..."). The chdir shim makes the bundle root the CWD, so we
# STAGE resources under both layouts and bundle them preserving those relative
# paths (mirrors the emcc --preload-file @/path scheme). Shared lib data that
# examples read by repo-root path is staged too.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ex_dir=${1:?usage: stage-app-assets.sh <project-dir> <app.c> <stage>}
app_c=${2:?usage: stage-app-assets.sh <project-dir> <app.c> <stage>}
stage=${3:?usage: stage-app-assets.sh <project-dir> <app.c> <stage>}
rm -rf "$stage"; mkdir -p "$stage"
if [ -d "$ex_dir/assets" ]; then
  mkdir -p "$stage/assets";  cp -R "$ex_dir/assets/." "$stage/assets/"  # assets/... (CWD-relative)
  # In-repo examples ALSO read their assets by repo-root path
  # (examples/<name>/assets/...), so stage that layout too. An out-of-tree
  # project (a game with its own repo, read root-relative "assets/..." and
  # "lib/...") is not under $root, where `${ex_dir#"$root/"}` would leave an
  # absolute path and bury the bundle under it — skip the repo-root layer there.
  case "$ex_dir/" in
    "$root"/*)
      ex_rel=${ex_dir#"$root/"}                 # examples/136_walker_character
      mkdir -p "$stage/$ex_rel"; cp -R "$ex_dir/assets" "$stage/$ex_rel/" ;;
  esac
fi
# Each package's declared `assets`, where Sys.packageAssetPath reads them on a device
package_lines=$(cd "$ex_dir" && "$root/compiler/bin/rae" tree --assets 2>/dev/null || true)
if [ -n "$package_lines" ]; then
  tab=$(printf '\t')
  while IFS="$tab" read -r package_name package_dir package_assets; do
    mkdir -p "$stage/packages/$package_name"
    for package_folder in $package_assets; do
      mkdir -p "$stage/packages/$package_name/$(dirname "$package_folder")"
      cp -R "$package_dir/$package_folder" "$stage/packages/$package_name/$(dirname "$package_folder")/"
    done
  done <<EOF
$package_lines
EOF
fi
for shared in lib/app3d/scenes lib/data lib/assets/walker; do
  if [ -e "$root/$shared" ]; then
    dst="$stage/$(dirname "$shared")"; mkdir -p "$dst"; cp -R "$root/$shared" "$dst/"
  fi
done
# Assets an app reads from ANOTHER in-repo example (118/119 draw their debug
# HUD with 136's font): every string the C names under examples/<x>/assets/ is
# staged at its repo-root path. A path built by interpolation is only a prefix
# in the C ("lib/assets/walker/Roboto-Regular" + ".mtsdf.json"),
# so every file starting with it is copied. On the desktop the CWD is the repo
# root and these resolve on their own; on a device only bundled files exist.
grep -oE '"examples/[A-Za-z0-9_]+/assets/[^"]*"' "$app_c" | tr -d '"' | sort -u \
  | while IFS= read -r ref; do
      for found in "$root/$ref"*; do
        [ -e "$found" ] || continue
        rel=${found#"$root/"}
        mkdir -p "$stage/$(dirname "$rel")"; cp -R "$found" "$stage/$rel"
      done
    done
# Shared WGSL that examples compose at RUNTIME (noise, world_biome, terrain_splat,
# ...). Bundle the whole lib/*.wgsl set: a missing one does not fail gracefully —
# the composed shader loses its entry point and wgpu-native ABORTS at pipeline
# creation (this is what crashed 136 on device: only noise.wgsl was shipped).
# Every lib/**/*.wgsl, at its own relative path (lib/gpu/..., lib/water/...):
# a stdlib read tries the CWD-relative "lib/..." first, and on a device there
# is no $RAE_STDLIB to fall back on.
(cd "$root" && find lib -name '*.wgsl') | while IFS= read -r wgsl; do
  mkdir -p "$stage/$(dirname "$wgsl")"; cp "$root/$wgsl" "$stage/$wgsl"
done
# The stdlib's own pictures (lib/assets: the grass tuft, test textures, the
# Material icon atlas and its .json), read the same way at "lib/assets/...";
# without them a device has no grass and no icons
(cd "$root" && find lib/assets \( -name '*.png' -o -name '*.mtsdf.json' \)) | while IFS= read -r picture; do
  mkdir -p "$stage/$(dirname "$picture")"; cp "$root/$picture" "$stage/$picture"
done
