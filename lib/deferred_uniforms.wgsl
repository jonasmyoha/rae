// The uniforms the deferred lighting pass and the forward main pass share
// (forward-rae-1): the light and sky (LightU, uploaded by uploadLightUniform in
// GbufferTargets.rae) and the sun cascades (ShadowU, ShadowMaps.rae). Structs
// only; each composition declares its own bindings.
struct LightU {
  invViewProj: mat4x4<f32>,
  camPos: vec4<f32>,
  sunDir: vec4<f32>,
  sunColor: vec4<f32>,
  ambSky: vec4<f32>,
  ambGround: vec4<f32>,
  clearColor: vec4<f32>,
  viewProj: mat4x4<f32>,
  skyParams: vec4<f32>,
  skyZenith: vec4<f32>,
  skyHorizon: vec4<f32>,
  hosek: array<vec4<f32>, 9>,
};
struct ShadowU {
  lightViewProj: array<mat4x4<f32>, 4>,
  splitFar: vec4<f32>,
  texelWorld: vec4<f32>,
  depthRange: vec4<f32>,
  shadowCfg: vec4<f32>,
};
