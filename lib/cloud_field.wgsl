// The slab tier's field map (lib/CloudSlab): the clouds' 2D field round the
// eye (share, how thick, where in its puff), the footprint the slab's ray
// march reads instead of evaluating the noise at every step
const PI = 3.14159265;  // sky_hosek.wgsl reads it
@group(0) @binding(0) var<uniform> L: LightU;
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let world = cloudFieldOrigin() + pos.xy * (cloudFieldSpan / cloudFieldTexels);
  let layer = skyCloudSample(skyCloudPoint(world), vec2<f32>(1.0, 0.0), 0.0);
  return vec4<f32>(layer.share, layer.thick, layer.away);
}
