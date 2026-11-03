// The clouds' shadow map (lib/CloudShadow, docs/cloud-rendering-research.md
// §8.2): how much of the key light the cloud layer lets through, over the
// clouds' plane round the eye (cloudShadowOrigin, sky_hosek.wgsl), from the
// same field the sky draws (skyCloudSunThrough). The last row's first texel
// is the map's average: the light let through and the share of cloud. A
// fullscreen pass into the map; composed after sky_hosek.wgsl and
// deferred_uniforms.wgsl. Every cloud tier writes this one map.
const PI = 3.14159265;  // sky_hosek.wgsl reads it
@group(0) @binding(0) var<uniform> L: LightU;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  // the cloud tier off (L.clouds.w 0, lib/CloudTier): no field, the sun
  // dimmed evenly by the cover (a fifth under a full one), as the weather
  // still asks
  if (L.clouds.w < 0.5) {
    let cover = clamp(L.clouds.x, 0.0, 1.0);
    return vec4<f32>(1.0 - 0.8 * cover, cover, 0.0, 1.0);
  }
  let texel = cloudShadowSpan / cloudShadowTexels;
  let origin = cloudShadowOrigin();
  if (pos.y < cloudShadowTexels) {
    let world = origin + pos.xy * texel;
    return vec4<f32>(skyCloudSunThrough(skyCloudPoint(world)), 0.0, 0.0, 1.0);
  }
  if (pos.x > 1.0 || L.clouds.x <= 0.001) { return vec4<f32>(1.0, 0.0, 0.0, 1.0); }
  // the average, from 8 x 8 points over the map
  var through = 0.0;
  var share = 0.0;
  for (var j = 0; j < 8; j = j + 1) {
    for (var i = 0; i < 8; i = i + 1) {
      let world = origin + (vec2<f32>(f32(i), f32(j)) + 0.5) / 8.0 * cloudShadowSpan;
      let layer = skyCloudSample(skyCloudPoint(world), vec2<f32>(1.0, 0.0), 0.0);
      through = through + skyCloudThrough(layer);
      share = share + layer.share;
    }
  }
  return vec4<f32>(through / 64.0, share / 64.0, 0.0, 1.0);
}
