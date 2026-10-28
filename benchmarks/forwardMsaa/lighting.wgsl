// The deferred lighting pass of the study (fullscreen): reads the G-buffer
// and depth, reconstructs the position, runs the same shadeSurface.
@group(0) @binding(2) var normalTex: texture_2d<f32>;
@group(0) @binding(3) var albedoTex: texture_2d<f32>;
@group(0) @binding(4) var materialTex: texture_2d<f32>;
@group(0) @binding(5) var depthTex: texture_depth_2d;
@vertex
fn vsFullscreen(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
@fragment
fn fsLighting(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let px = vec2<i32>(pos.xy);
  let depth = textureLoad(depthTex, px, 0);
  if (depth >= 1.0) { return skyColor; }
  let dims = vec2<f32>(textureDimensions(depthTex));
  let uv = (vec2<f32>(px) + vec2<f32>(0.5)) / dims;
  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  let w = F.invViewProj * ndc;
  let world = w.xyz / w.w;
  let N = normalize(textureLoad(normalTex, px, 0).xyz * 2.0 - vec3<f32>(1.0));
  let albedo = textureLoad(albedoTex, px, 0);
  let material = textureLoad(materialTex, px, 0);
  return toneMap(shadeSurface(N, world, albedo.rgb, albedo.a) * material.a);
}
