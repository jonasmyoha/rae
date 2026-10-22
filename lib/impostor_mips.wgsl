// The impostor atlas's mips (lib/ImpostorBake.rae), coverage-preserving
// (Castano, "Computing alpha mipmaps", 2010; docs/tree-lod-design.md §2.1):
// each level's alpha is scaled per view cell so that an alpha cut at the
// cutoff keeps the same share of the cell as level 0 does. Level `k` is made
// straight from level 0 (each of its texels averages a block of 2^k x 2^k),
// so no scaling compounds, in three passes:
//
//   countLevel  each texel's averaged alpha, as a key, into its cell's
//               histogram; and the cell's covered texels at level 0
//   chooseScale per cell, the key the target share of texels reaches, and
//               the scale that brings that key to the cutoff
//   writeLevel  the level, alpha times its cell's scale
//
// A block's averaged alpha takes few values (a level-0 alpha is 0 or 1), so
// many texels tie; the key adds a fixed per-texel offset below one step
// between those values, which orders the ties without crossing a step, so the
// share can be met almost exactly (the CPU sprite bake's jitter, made exact).
struct MipU {
  // level, view cells per layer side (every page's views), a cell's size
  // at level 0, histogram bins
  a: vec4<u32>,
  // x = the alpha cutoff, y = layers, z = a cell's height at level 0 (its
  // width is a.z)
  b: vec4<f32>,
};
@group(0) @binding(0) var<uniform> U: MipU;
@group(0) @binding(1) var sourceAlbedo: texture_2d_array<f32>;
@group(0) @binding(2) var sourceNormalDepth: texture_2d_array<f32>;
// Per layer and cell: U.a.w bins of keys
@group(0) @binding(3) var<storage, read_write> histogram: array<atomic<u32>>;
// Per layer and cell: its texels at level 0 whose alpha reaches the cutoff
@group(0) @binding(4) var<storage, read_write> covered: array<atomic<u32>>;
// Per layer and cell: the scale of the level's alpha
@group(0) @binding(5) var<storage, read_write> scales: array<f32>;
@group(0) @binding(6) var targetAlbedo: texture_storage_2d_array<rgba8unorm, write>;
@group(0) @binding(7) var targetNormalDepth: texture_storage_2d_array<rgba8unorm, write>;

fn levelSize() -> vec2<u32> {
  return vec2<u32>((U.a.y * U.a.z) >> U.a.x, (U.a.y * u32(U.b.z)) >> U.a.x);
}

fn blockSize() -> u32 {
  return 1u << U.a.x;
}

fn cellOf(texel: vec2<u32>, layer: u32) -> u32 {
  let cellSize = vec2<u32>(U.a.z >> U.a.x, u32(U.b.z) >> U.a.x);
  let cell = texel / cellSize;
  return (layer * U.a.y + cell.y) * U.a.y + cell.x;
}

// A fixed offset (0..1) per texel of the level
fn tieBreak(texel: vec2<u32>, layer: u32) -> f32 {
  var h = texel.x * 1973u + texel.y * 9277u + layer * 26699u + U.a.x * 104729u;
  h = (h ^ (h >> 13u)) * 1274126177u;
  h = h ^ (h >> 16u);
  return f32(h & 65535u) / 65536.0;
}

// The texel's alpha summed over its block at level 0, and how many of the
// block's texels reach the cutoff
struct BlockAlpha {
  sum: f32,
  covered: u32,
};
fn blockAlpha(texel: vec2<u32>, layer: u32) -> BlockAlpha {
  let size = blockSize();
  var result: BlockAlpha;
  result.sum = 0.0;
  result.covered = 0u;
  for (var y = 0u; y < size; y = y + 1u) {
    for (var x = 0u; x < size; x = x + 1u) {
      let source = vec2<i32>(texel * size + vec2<u32>(x, y));
      let alpha = textureLoad(sourceAlbedo, source, i32(layer), 0).a;
      result.sum = result.sum + alpha;
      if (alpha >= U.b.x) {
        result.covered = result.covered + 1u;
      }
    }
  }
  return result;
}

// The key (0..1) of an alpha summed over a block of `count` texels
fn alphaKey(sum: f32, count: f32, texel: vec2<u32>, layer: u32) -> f32 {
  return (sum + tieBreak(texel, layer)) / (count + 1.0);
}

@compute @workgroup_size(8, 8, 1)
fn countLevel(@builtin(global_invocation_id) gid: vec3<u32>) {
  let size = levelSize();
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }
  let texel = gid.xy;
  let layer = gid.z;
  let block = blockAlpha(texel, layer);
  let count = f32(blockSize() * blockSize());
  let key = alphaKey(block.sum, count, texel, layer);
  let bins = U.a.w;
  let bin = min(u32(key * f32(bins)), bins - 1u);
  let cell = cellOf(texel, layer);
  atomicAdd(&histogram[cell * bins + bin], 1u);
  atomicAdd(&covered[cell], block.covered);
}

@compute @workgroup_size(64, 1, 1)
fn chooseScale(@builtin(global_invocation_id) gid: vec3<u32>) {
  let cells = U.a.y * U.a.y * u32(U.b.y);
  let cell = gid.x;
  if (cell >= cells) {
    return;
  }
  // The texels of the cell at this level that should pass the cut: level 0's
  // share of covered texels
  let block = f32(blockSize() * blockSize());
  let wanted = f32(atomicLoad(&covered[cell])) / block;
  let bins = U.a.w;
  if (wanted < 0.5) {
    scales[cell] = 1.0;
    return;
  }
  // From the top bin down, the bin whose lower edge keeps nearest the target
  var kept = 0.0;
  var chosen = 0u;
  for (var i = 0u; i < bins; i = i + 1u) {
    let bin = bins - 1u - i;
    let before = kept;
    kept = kept + f32(atomicLoad(&histogram[cell * bins + bin]));
    if (kept >= wanted) {
      chosen = bin;
      if (wanted - before < kept - wanted && bin + 1u < bins) {
        chosen = bin + 1u;
      }
      break;
    }
  }
  let threshold = max(f32(chosen) / f32(bins), 0.0001);
  scales[cell] = U.b.x / threshold;
}

@compute @workgroup_size(8, 8, 1)
fn writeLevel(@builtin(global_invocation_id) gid: vec3<u32>) {
  let size = levelSize();
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }
  let texel = gid.xy;
  let layer = gid.z;
  let blockWidth = blockSize();
  // The block's colour, normal and depth, each weighted by alpha: a
  // see-through texel holds none of them
  var albedo = vec3<f32>(0.0);
  var normal = vec3<f32>(0.0);
  var behind = 0.0;
  var depth = 0.0;
  var sum = 0.0;
  for (var y = 0u; y < blockWidth; y = y + 1u) {
    for (var x = 0u; x < blockWidth; x = x + 1u) {
      let source = vec2<i32>(texel * blockWidth + vec2<u32>(x, y));
      let colour = textureLoad(sourceAlbedo, source, i32(layer), 0);
      let normalDepth = textureLoad(sourceNormalDepth, source, i32(layer), 0);
      albedo = albedo + colour.rgb * colour.a;
      normal = normal + impostorUnpackNormal(normalDepth.xy) * colour.a;
      behind = behind + normalDepth.z * colour.a;
      depth = depth + normalDepth.w * colour.a;
      sum = sum + colour.a;
    }
  }
  let destination = vec2<i32>(texel);
  if (sum <= 0.0) {
    textureStore(targetAlbedo, destination, i32(layer), vec4<f32>(0.0));
    textureStore(targetNormalDepth, destination, i32(layer), vec4<f32>(0.5, 0.5, 0.0, 0.0));
    return;
  }
  let count = f32(blockWidth * blockWidth);
  let key = alphaKey(sum, count, texel, layer);
  let alpha = clamp(key * scales[cellOf(texel, layer)], 0.0, 1.0);
  var n = normal / max(length(normal), 0.0001);
  textureStore(targetAlbedo, destination, i32(layer), vec4<f32>(albedo / sum, alpha));
  textureStore(
    targetNormalDepth, destination, i32(layer),
    vec4<f32>(impostorPackNormal(n), behind / sum * alpha, depth / sum * alpha));
}
