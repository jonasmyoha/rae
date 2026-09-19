@group(0) @binding(0) var<uniform> P: vec4<f32>;
@group(0) @binding(1) var gbaTex: texture_2d<f32>;
@group(0) @binding(2) var gbbTex: texture_2d<f32>;
@group(0) @binding(3) var gbcTex: texture_2d<f32>;
@group(0) @binding(4) var depthTex: texture_depth_2d;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let px = vec2<i32>(pos.xy);
  let mode = i32(P.x);
  var c = vec3<f32>(0.0);
  if (mode == 2) {
    let n = octDecode(textureLoad(gbaTex, px, 0).xy);
    c = n * 0.5 + vec3<f32>(0.5);
  } else if (mode == 3) {
    let gbb = textureLoad(gbbTex, px, 0);
    let gbc = textureLoad(gbcTex, px, 0);
    c = vec3<f32>(gbc.z, gbb.a, gbc.w);
  } else if (mode == 4) {
    let d = textureLoad(depthTex, px, 0);
    let zn = P.y; let zf = P.z;
    let lin = (zf * zn) / max(d * (zf - zn) + zn, 1e-6);
    c = vec3<f32>(clamp((lin - zn) / max(zf - zn, 1e-6), 0.0, 1.0));
  } else {
    c = textureLoad(gbbTex, px, 0).rgb;
  }
  return vec4<f32>(pow(c, vec3<f32>(1.0 / 2.2)), 1.0);
}
