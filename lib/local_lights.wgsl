// The local lights' pass (lib/LocalLights): spots and point lights added onto
// the lit target, one instanced quad a light over the screen rectangle of the
// sphere its reach fits in, each pixel's surface read from the G-buffer. No
// shadows. Composed after gbuffer_octahedral.wgsl and deferred_uniforms.wgsl.
const PI = 3.14159265;
struct LocalLightU {
  // xyz where it is, w its range (m)
  place: vec4<f32>,
  // xyz its way, w the cosine of its cone's edge (-1: a point light)
  way: vec4<f32>,
  // rgb candela, w the cosine from where inward the cone is full
  color: vec4<f32>,
  // the sphere its reach fits in
  bounds: vec4<f32>,
};
struct LocalLightsU { lights: array<LocalLightU, 32> };
@group(0) @binding(0) var<uniform> L: LightU;
@group(0) @binding(1) var<uniform> S: LocalLightsU;
@group(0) @binding(2) var gbaTex: texture_2d<f32>;
@group(0) @binding(3) var gbbTex: texture_2d<f32>;
@group(0) @binding(4) var gbcTex: texture_2d<f32>;
@group(0) @binding(5) var depthTex: texture_depth_2d;
struct LocalOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) @interpolate(flat) light: u32,
};
@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) index: u32) -> LocalOut {
  let bounds = S.lights[index].bounds;
  // the rectangle of the sphere's box; the whole screen when the eye is in it
  // or a corner is behind the eye
  var low = vec2<f32>(1.0);
  var high = vec2<f32>(-1.0);
  var whole = length(L.camPos.xyz - bounds.xyz) < bounds.w * 1.8;
  for (var i = 0u; i < 8u; i = i + 1u) {
    let corner = bounds.xyz + bounds.w * vec3<f32>(
      select(-1.0, 1.0, (i & 1u) != 0u), select(-1.0, 1.0, (i & 2u) != 0u), select(-1.0, 1.0, (i & 4u) != 0u));
    let clip = L.viewProj * vec4<f32>(corner, 1.0);
    if (clip.w <= 0.01) {
      whole = true;
    } else {
      low = min(low, clip.xy / clip.w);
      high = max(high, clip.xy / clip.w);
    }
  }
  if (whole) {
    low = vec2<f32>(-1.0);
    high = vec2<f32>(1.0);
  }
  var corners = array<vec2<f32>, 6>(
    vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),
    vec2<f32>(0.0, 1.0), vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0));
  var out: LocalOut;
  out.pos = vec4<f32>(mix(clamp(low, vec2<f32>(-1.0), vec2<f32>(1.0)), clamp(high, vec2<f32>(-1.0), vec2<f32>(1.0)), corners[vi]), 0.0, 1.0);
  out.light = index;
  return out;
}
// How much of the light gets through the mist from `wpos` to the eye
// (deferred_shade.wgsl applyMist's density)
fn localMistThrough(wpos: vec3<f32>) -> f32 {
  if (L.mist.w <= 0.5) { return 1.0; }
  let toP = wpos - L.camPos.xyz;
  let falloff = max(L.mist.y, 0.00001);
  let atEye = L.mist.x * exp(clamp(-falloff * (L.camPos.z - L.mist.z), -40.0, 20.0));
  let climb = clamp(falloff * toP.z, -20.0, 40.0);
  var depth = atEye * length(toP);
  if (abs(climb) > 0.0001) { depth = depth * (1.0 - exp(-climb)) / climb; }
  return exp(-max(depth, 0.0));
}
@fragment
fn fs(in: LocalOut) -> @location(0) vec4<f32> {
  let px = vec2<i32>(in.pos.xy);
  let depth = textureLoad(depthTex, px, 0);
  if (depth <= 0.0) { return vec4<f32>(0.0); }
  let dim = vec2<f32>(textureDimensions(gbbTex, 0));
  let uv = (vec2<f32>(px) + vec2<f32>(0.5)) / dim;
  let wpos4 = L.invViewProj * vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  let wpos = wpos4.xyz / wpos4.w;
  let light = S.lights[in.light];
  let toLight = light.place.xyz - wpos;
  let far = length(toLight);
  if (far >= light.place.w) { return vec4<f32>(0.0); }
  let way = toLight / max(far, 1e-4);
  var cone = 1.0;
  if (light.way.w > -1.0) {
    cone = smoothstep(light.way.w, max(light.color.w, light.way.w + 1e-4), dot(-way, light.way.xyz));
    if (cone <= 0.0) { return vec4<f32>(0.0); }
  }
  let gba = textureLoad(gbaTex, px, 0);
  let mode = gba.w;
  // unlit surfaces
  if (mode > 0.5 && mode < 0.83) { return vec4<f32>(0.0); }
  let N = octDecode(gba.xy);
  let facing = dot(N, way);
  if (facing <= 0.0) { return vec4<f32>(0.0); }
  let gbb = textureLoad(gbbTex, px, 0);
  let gbc = textureLoad(gbcTex, px, 0);
  let edge = far / light.place.w;
  let window = clamp(1.0 - edge * edge * edge * edge, 0.0, 1.0);
  // lux on a surface facing it, exposed
  let lux = light.color.rgb * (L.camPos.w * cone * window * window / (far * far + 0.04));
  var lit: vec3<f32>;
  if (mode > 0.83) {
    // toon: the sun's share (deferred_shade.wgsl), in one soft step
    lit = gbb.rgb * 0.34 * smoothstep(0.0, 0.25, facing);
  } else {
    let rough = clamp(gbb.a, 0.1, 1.0);
    let metallic = clamp(gbc.z, 0.0, 1.0);
    let V = normalize(L.camPos.xyz - wpos);
    let H = normalize(V + way);
    let NoV = max(dot(N, V), 1e-3);
    let NoH = max(dot(N, H), 0.0);
    let f0 = mix(vec3<f32>(0.04), gbb.rgb, metallic);
    let fresnelF = f0 + (vec3<f32>(1.0) - f0) * pow(1.0 - max(dot(V, H), 0.0), 5.0);
    let a2 = rough * rough * rough * rough;
    let d = NoH * NoH * (a2 - 1.0) + 1.0;
    let k = (rough + 1.0) * (rough + 1.0) / 8.0;
    // the GGX lobe over 4 NoV NoL, with Schlick's G for both
    let lobe = a2 / (PI * d * d + 1e-5) / (4.0 * (NoV * (1.0 - k) + k) * (facing * (1.0 - k) + k));
    lit = (gbb.rgb * (1.0 - metallic) * (vec3<f32>(1.0) - fresnelF) / PI + fresnelF * lobe) * facing;
  }
  let add = min(lit * lux * localMistThrough(wpos), vec3<f32>(60000.0));
  return vec4<f32>(add, 0.0);
}
