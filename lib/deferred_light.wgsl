// The deferred lighting pass: reads the G-buffer, depth and the half-size SSAO
// for each pixel and shades it with shadeSurface (deferred_shade.wgsl, composed
// in front of this file).
@group(0) @binding(0) var<uniform> L: LightU;
@group(0) @binding(1) var gbaTex: texture_2d<f32>;
@group(0) @binding(2) var gbbTex: texture_2d<f32>;
@group(0) @binding(3) var gbcTex: texture_2d<f32>;
@group(0) @binding(4) var depthTex: texture_depth_2d;
@group(0) @binding(5) var aoTex: texture_2d<f32>;
@group(0) @binding(6) var<uniform> SH: ShadowU;
@group(0) @binding(7) var shadowTex: texture_depth_2d_array;
@group(0) @binding(8) var shadowSamp: sampler_comparison;
// The SSAO's half-resolution depth (deferred_ao_depth.wgsl), for the upsample.
@group(0) @binding(9) var aoDepthTex: texture_2d<f32>;
// The clouds' shadow map (lib/CloudShadow; cloudSunLight in deferred_shade.wgsl)
@group(0) @binding(10) var cloudShadowTex: texture_2d<f32>;
// The slab tier's half-resolution clouds (lib/CloudSlab): premultiplied light
// and how much of the sky they hide, upsampled over the sky
@group(0) @binding(11) var cloudSlabTex: texture_2d<f32>;
fn cloudSlabAt(uv: vec2<f32>) -> vec4<f32> {
  let size = vec2<f32>(textureDimensions(cloudSlabTex, 0));
  let at = uv * size - 0.5;
  let cell = clamp(vec2<i32>(floor(at)), vec2<i32>(0), vec2<i32>(size) - vec2<i32>(2));
  let f = clamp(at - vec2<f32>(cell), vec2<f32>(0.0), vec2<f32>(1.0));
  let a = textureLoad(cloudSlabTex, cell, 0);
  let b = textureLoad(cloudSlabTex, cell + vec2<i32>(1, 0), 0);
  let c = textureLoad(cloudSlabTex, cell + vec2<i32>(0, 1), 0);
  let d = textureLoad(cloudSlabTex, cell + vec2<i32>(1, 1), 0);
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let px = vec2<i32>(pos.xy);
  let depth = textureLoad(depthTex, px, 0);
  if (depth <= 0.0) {
    let uvF = (vec2<f32>(px) + vec2<f32>(0.5)) / vec2<f32>(textureDimensions(gbbTex, 0));
    let ndcF = vec4<f32>(uvF.x * 2.0 - 1.0, 1.0 - uvF.y * 2.0, 0.0, 1.0);
    if (L.clearColor.w > 0.5) { return vec4<f32>(L.clearColor.rgb, 1.0); }
    let farW = L.invViewProj * ndcF;
    let dirF = normalize(farW.xyz / farW.w - L.camPos.xyz);
    // the slab tier (L.clouds.w 2 and up): its clouds over the sky
    skySlabActive = L.clouds.w > 1.5 && L.clouds.x > 0.001;
    var skyLit = skyColor(dirF);
    if (skySlabActive) {
      let slab = cloudSlabAt(uvF);
      skyLit = skyLit * (1.0 - slab.w) + slab.rgb;
    }
    return vec4<f32>(skyLit, 1.0);
  }
  let gbb = textureLoad(gbbTex, px, 0);
  let gba = textureLoad(gbaTex, px, 0);
  let mode = gba.w;
  let isEmissive = mode > 0.16 && mode < 0.5;
  if (mode > 0.5 && mode < 0.83) {
    return vec4<f32>(gbb.rgb, 1.0);
  }
  let dim = vec2<f32>(textureDimensions(gbbTex, 0));
  let uv = (vec2<f32>(px) + vec2<f32>(0.5)) / dim;
  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  let wpos4 = L.invViewProj * ndc;
  let gbc = textureLoad(gbcTex, px, 0);
  var surface: ShadedSurface;
  surface.wpos = wpos4.xyz / wpos4.w;
  surface.N = octDecode(gba.xy);
  surface.albedo = gbb.rgb;
  surface.metallic = gbc.z;
  surface.rough = gbb.a;
  surface.ao = gbc.w;
  surface.mode = mode;
  surface.noRim = select(0.0, 1.0, gba.z < 0.25);
  surface.emissive = 0.0;
  if (isEmissive) {
    surface.emissive = exp(gbc.w * 6.91) - 1.0;
    surface.ao = 1.0;
  }
  surface.ssao = ssaoAt(px, depth);
  return vec4<f32>(shadeSurface(surface), 1.0);
}
