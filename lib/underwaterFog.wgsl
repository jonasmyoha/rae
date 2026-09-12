// underwaterFog.wgsl — the underwater tint/fog pass (#855, water Phase 3).
//
// Runs after the water surface and the transparents were composed into the
// lit HDR target and before TAA / composite, only while the camera is under a
// body's surface (the CPU decides: water/Underwater.rae). A fullscreen pass
// that reads the frame's opaque+transparent radiance (the litCopy snapshot
// taken just before) and the G-buffer depth, reconstructs the world position
// under each pixel, measures the UNDERWATER view distance — the ray from the
// camera to that point, cut at the still surface plane when the point is
// above the water (looking up through the surface) — and applies Beer-Lambert
// attenuation per channel toward the body's deep colour (in-scattering), so
// far things sink into the water colour and near things keep their own.
struct Underwater {
  invViewProj: mat4x4<f32>,   // the frame's UNJITTERED clip -> world
  camera: vec4<f32>,          // xyz = camera position, w = surface z at the camera
  deep: vec4<f32>,            // rgb = body deep colour (the fog's asymptote), w = far distance cap (m)
  absorb: vec4<f32>,          // rgb = Beer-Lambert absorption per metre, w = density scale
  params: vec4<f32>,          // x = near-surface tint strength, y = tint depth (m), zw = unused
};
@group(0) @binding(0) var<uniform> U: Underwater;
@group(0) @binding(1) var sceneTex: texture_2d<f32>;    // litCopy: the composed frame
@group(0) @binding(2) var depthTex: texture_depth_2d;   // reverse-Z scene depth

struct VsOut {
  @builtin(position) pos: vec4<f32>,
};

@vertex
fn vs(@builtin(vertex_index) vi: u32) -> VsOut {
  // One triangle covering the screen: (-1,-1) (3,-1) (-1,3).
  var o: VsOut;
  let x = f32(i32(vi & 1u) * 4 - 1);
  let y = f32(i32(vi >> 1u) * 4 - 1);
  o.pos = vec4<f32>(x, y, 0.0, 1.0);
  return o;
}

@fragment
fn fs(i: VsOut) -> @location(0) vec4<f32> {
  let dims = vec2<i32>(textureDimensions(sceneTex));
  let px = clamp(vec2<i32>(i32(i.pos.x), i32(i.pos.y)), vec2<i32>(0), dims - vec2<i32>(1));
  let src = textureLoad(sceneTex, px, 0).rgb;
  let ndcDepth = textureLoad(depthTex, px, 0);
  // Pixel centre -> NDC (y up), through the inverse view-projection. Under
  // reverse-Z a depth of 0 is the far plane, which is exactly where a sky
  // pixel's fog should saturate; no special case is needed.
  let uv = (vec2<f32>(px) + vec2<f32>(0.5)) / vec2<f32>(dims);
  let ndc = vec2<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  let clip = vec4<f32>(ndc, ndcDepth, 1.0);
  let worldH = U.invViewProj * clip;
  var world = worldH.xyz / max(abs(worldH.w), 1e-6) * sign(worldH.w);
  let cam = U.camera.xyz;
  let surfaceZ = U.camera.w;
  // Above the surface: only the segment of the ray still under water counts.
  var ray = world - cam;
  if (world.z > surfaceZ && ray.z > 1e-6) {
    let t = clamp((surfaceZ - cam.z) / ray.z, 0.0, 1.0);
    ray = ray * t;
  }
  let pathLength = min(length(ray), U.deep.w);
  // Beer-Lambert per channel toward the deep colour: red is absorbed first,
  // so the far field goes blue-green before it goes dark.
  let transmit = exp(-U.absorb.rgb * pathLength * U.absorb.w);
  var colour = src * transmit + U.deep.rgb * (vec3<f32>(1.0) - transmit);
  // A light tint everywhere (the water between the eye and the near plane)
  // that fades with the camera's own depth so a shallow dip reads as water.
  let camDepth = max(surfaceZ - cam.z, 0.0);
  let tint = U.params.x * clamp(camDepth / max(U.params.y, 0.01), 0.0, 1.0);
  colour = mix(colour, colour * U.deep.rgb * 4.0, tint * 0.25);
  return vec4<f32>(colour, 1.0);
}
