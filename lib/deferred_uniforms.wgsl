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
  // Mist (RendererMist): x density per meter at the base height, y how fast
  // it thins per meter up, z the base height (world z), w on (1) / off (0);
  // mistTint: rgb a tint on the sky's color, w how much of that color's
  // saturation the mist loses (mist is paler than the sky)
  mist: vec4<f32>,
  mistTint: vec4<f32>,
  // The sky's sun (xyz: toward it; w: the moon's angular radius) — the key
  // light (sunDir) is the moon at night, the sky's sun stays the sun; the moon
  // (xyz: toward it; w: its disc's intensity, 0 = no moon); the night sky
  // (x: its moonlight 0..1)
  skySun: vec4<f32>,
  moon: vec4<f32>,
  night: vec4<f32>,
  // The extra moons (Sky.moons, 3): per moon xyz toward it and w its disc's
  // angular radius (0: none), then rgb its color times its luminance (cd/m2);
  // moons[1].w: how much of the sun the earth's moon covers (an eclipse)
  moons: array<vec4<f32>, 6>,
};
struct ShadowU {
  lightViewProj: array<mat4x4<f32>, 4>,
  splitFar: vec4<f32>,
  texelWorld: vec4<f32>,
  depthRange: vec4<f32>,
  shadowCfg: vec4<f32>,
};
