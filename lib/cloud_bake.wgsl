// The baked clouds' bake (lib/CloudBake): one tile of the sky map, marched
// through the volumetric clouds (cloud_volume.wgsl volumeMarch) with the light,
// the eye and the wind the bake began with (L is that frame's own copy, so the
// tiles of many frames fit together). The instance is the tile. A texel takes
// four rays, each with its own place in the texel and in the first step: they
// average out the march's grain, which no history does here. The light is
// stored with the exposure (camPos.w) it began with, and the lighting pass
// scales it by the exposure's change since (cloudBake[3]): before the exposure
// the clouds by the sun are past what a half float holds. Held finite
// (hdrFinite): a bad texel would stay a whole bake long. SlabU: pattern.w the steps; sizes.x the tile's side, sizes.y
// the map's, sizes.z its tiles in a row.
const PI = 3.14159265;  // sky_hosek.wgsl reads it
@group(0) @binding(0) var<uniform> L: LightU;
@group(0) @binding(1) var<uniform> S: SlabU;
@group(0) @binding(2) var fieldTex: texture_2d<f32>;
@group(0) @binding(3) var lutTex: texture_2d<f32>;
struct BakeOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) @interpolate(flat) tile: u32,
};
@vertex
fn vsTile(@builtin(vertex_index) vi: u32, @builtin(instance_index) tile: u32) -> BakeOut {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  var out: BakeOut;
  out.pos = vec4<f32>(points[vi], 0.0, 1.0);
  out.tile = tile;
  return out;
}
@fragment
fn fs(in: BakeOut) -> @location(0) vec4<f32> {
  skySlabActive = true;
  let row = u32(S.sizes.z);
  let corner = vec2<f32>(f32(in.tile % row), f32(in.tile / row)) * S.sizes.x;
  let texel = corner + floor(in.pos.xy);
  let grain = fract(sin(dot(texel, vec2<f32>(12.9898, 78.233))) * 43758.5453);
  var sum = vec4<f32>(0.0);
  for (var i = 0; i < 4; i = i + 1) {
    let at = texel + vec2<f32>(0.25 + 0.5 * f32(i % 2), 0.25 + 0.5 * f32(i / 2));
    let dir = cloudMapDir(at / S.sizes.y);
    sum = sum + volumeMarch(dir, fract(grain + f32(i) * 0.25), clamp(i32(S.pattern.w), 16, 128));
  }
  sum = sum * 0.25;
  return vec4<f32>(hdrFinite(sum.rgb), hdrFinite(vec3<f32>(sum.w)).x);
}
