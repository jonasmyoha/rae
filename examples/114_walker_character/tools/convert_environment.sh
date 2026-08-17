#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 /path/to/walker/Assets" >&2
  exit 2
fi

assets_root=$1
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output_dir="$script_dir/../assets/environment"
blender=/Applications/Blender.app/Contents/MacOS/Blender

convert_prop() {
  source_path=$1
  output_name=$2
  "$blender" -b --python "$script_dir/fbx_static_to_glb.py" -- \
    "$assets_root/$source_path" "$output_dir/$output_name"
}

convert_tree() {
  source_path=$1
  output_stem=$2
  "$blender" -b --python "$script_dir/fbx_tree_to_glb.py" -- \
    "$assets_root/$source_path" \
    "$output_dir/${output_stem}_trunk.glb" \
    "$output_dir/${output_stem}_crown.glb"
}

forest="assetstore/free_lowpoly_forest_nature_pack/Asset/Models"
wanderer="assetstore/FlatKit/Demos/[Demo] Wanderer/Wanderer-Models"
island="assetstore/FlatKit/Demos/[Demo] IslandWithTrees/IslandWithTrees-Models"

convert_tree "$forest/lowpoly_tree_001.fbx" tree_broad
convert_tree "$forest/lowpoly_tree_005.fbx" tree_tall
convert_prop "$forest/lowpoly_rock_001.fbx" rock.glb
convert_prop "$forest/lowpoly_flower_001.fbx" flower.glb
convert_prop "$wanderer/Bush01.fbx" bush.glb
convert_prop "$island/GrassBunch-v1-03.fbx" grass.glb
