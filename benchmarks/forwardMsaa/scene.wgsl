// The forward-vs-deferred study's scene (render-perf-14, benchmarks only):
// instanced spheres over a ground, one sun with GGX specular + a hemisphere
// ambient. The SAME lighting function runs in the deferred lighting pass and
// in the forward pass, so the two paths differ only in where it runs.
struct Frame {
  viewProj: mat4x4<f32>,
  invViewProj: mat4x4<f32>,
  camPos: vec4<f32>,
  sunDir: vec4<f32>,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> instances: array<vec4<f32>>;  // xyz position, w scale; then rgb colour, roughness
struct VsOut {
  @invariant @builtin(position) pos: vec4<f32>,
  @location(0) normal: vec3<f32>,
  @location(1) world: vec3<f32>,
  @location(2) albedo: vec3<f32>,
  @location(3) rough: f32,
};
@vertex
fn vsScene(@location(0) p: vec3<f32>, @location(1) n: vec3<f32>, @builtin(instance_index) ii: u32) -> VsOut {
  let placement = instances[ii * 2u];
  let material = instances[ii * 2u + 1u];
  let world = p * placement.w + placement.xyz;
  var o: VsOut;
  o.pos = F.viewProj * vec4<f32>(world, 1.0);
  o.normal = n;
  o.world = world;
  o.albedo = material.rgb;
  o.rough = material.a;
  return o;
}
fn shadeSurface(N: vec3<f32>, world: vec3<f32>, albedo: vec3<f32>, rough: f32) -> vec3<f32> {
  let V = normalize(F.camPos.xyz - world);
  let L = normalize(-F.sunDir.xyz);
  let H = normalize(V + L);
  let NoL = max(dot(N, L), 0.0);
  let NoV = max(dot(N, V), 1e-4);
  let NoH = max(dot(N, H), 0.0);
  let a = rough * rough;
  let a2 = a * a;
  let d = NoH * NoH * (a2 - 1.0) + 1.0;
  let D = a2 / (3.14159265 * d * d + 1e-5);
  let k = (rough + 1.0) * (rough + 1.0) / 8.0;
  let G = (NoV / (NoV * (1.0 - k) + k)) * (NoL / (NoL * (1.0 - k) + k));
  let F0 = vec3<f32>(0.04);
  let Fs = F0 + (vec3<f32>(1.0) - F0) * pow(1.0 - max(dot(V, H), 0.0), 5.0);
  let spec = D * G * Fs / max(4.0 * NoV * NoL, 1e-4);
  let diffuse = (vec3<f32>(1.0) - Fs) * albedo / 3.14159265;
  let hemi = mix(vec3<f32>(0.15, 0.12, 0.1), vec3<f32>(0.35, 0.45, 0.6), N.z * 0.5 + 0.5);
  return (diffuse + spec) * vec3<f32>(3.0, 2.8, 2.5) * NoL + hemi * albedo;
}
// Both paths write the same 8-bit image (Reinhard), so they compare pixel for pixel
fn toneMap(c: vec3<f32>) -> vec4<f32> {
  return vec4<f32>(c / (vec3<f32>(1.0) + c), 1.0);
}
const skyColour: vec4<f32> = vec4<f32>(0.3, 0.4, 0.55, 1.0);
// --- forward: shade in the geometry pass --------------------------------------
@fragment
fn fsForward(in: VsOut) -> @location(0) vec4<f32> {
  return toneMap(shadeSurface(normalize(in.normal), in.world, in.albedo, in.rough));
}
// The depth pre-pass: depth only (the colour write mask is empty)
@fragment
fn fsDepthOnly() -> @location(0) vec4<f32> {
  return vec4<f32>(0.0);
}
// --- deferred: write the G-buffer, shade in a fullscreen pass ---------------------
struct GbufferOut {
  @location(0) normal: vec4<f32>,
  @location(1) albedo: vec4<f32>,
  @location(2) material: vec4<f32>,
};
@fragment
fn fsGbuffer(in: VsOut) -> GbufferOut {
  var o: GbufferOut;
  o.normal = vec4<f32>(normalize(in.normal) * 0.5 + vec3<f32>(0.5), 1.0);
  o.albedo = vec4<f32>(in.albedo, in.rough);
  o.material = vec4<f32>(0.0, 0.0, 0.0, 1.0);
  return o;
}
