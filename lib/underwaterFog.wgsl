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
  sun: vec4<f32>,             // xyz = direction TOWARD the sun (unit), w = caustics on (1) / off (0)
  caustic: vec4<f32>,         // rgb = sun colour (caustic tint), w = caustic strength
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

// A compact self-contained value noise (this file is read verbatim, not
// composed with lib/noise.wgsl). Hash -> [0,1], smooth-interpolated.
fn cwHash(p: vec2<i32>) -> f32 {
  var h = u32(p.x) * 0x85ebca6bu ^ u32(p.y) * 0xc2b2ae35u;
  h = h ^ (h >> 15u); h = h * 0x2c1b3c6du; h = h ^ (h >> 12u);
  return f32(h & 0xffffffu) / 16777215.0;
}
fn cwValue(p: vec2<f32>) -> f32 {
  let c = vec2<i32>(floor(p));
  let f = fract(p);
  let u = f * f * (3.0 - 2.0 * f);
  let a = mix(cwHash(c), cwHash(c + vec2<i32>(1, 0)), u.x);
  let b = mix(cwHash(c + vec2<i32>(0, 1)), cwHash(c + vec2<i32>(1, 1)), u.x);
  return mix(a, b, u.y);
}
// Caustics (#860): a two-octave RIDGED noise (1 - |2n-1| peaks along thin
// curves, like the light network on a pool floor), the two octaves panned in
// opposite directions so the pattern shimmers instead of scrolling.
fn causticPattern(xy: vec2<f32>, t: f32) -> f32 {
  let a = 1.0 - abs(2.0 * cwValue(xy * 0.6 + vec2<f32>(t * 0.08, -t * 0.05)) - 1.0);
  let b = 1.0 - abs(2.0 * cwValue(xy * 1.13 + vec2<f32>(-t * 0.06, t * 0.09)) - 1.0);
  let net = a * b;
  // Sharpen: only the brightest ridges survive, so it reads as focused light.
  return pow(clamp(net, 0.0, 1.0), 3.0);
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

  // Sun caustics on SUBMERGED geometry only. Conditions, each of which zeroes
  // the contribution: caustics off; the sun below the horizon (toSun.z <= 0,
  // so no light enters — "without sun illumination" adds nothing); a sky pixel
  // (reverse-Z depth ~0, no surface to light); a surface AT or ABOVE the water
  // line (world.z >= surfaceZ — nothing above/outside the water is touched).
  let toSun = U.sun.xyz;
  if (U.sun.w > 0.5 && toSun.z > 0.0 && ndcDepth > 1e-6 && world.z < surfaceZ - 0.02) {
    // Project the submerged point up the sun ray onto the surface plane: every
    // point the same refracted shaft lights shares this coordinate, so the
    // caustic sits in world space and does not swim with the camera.
    let toSurface = (surfaceZ - world.z) / toSun.z;
    let causticXy = world.xy + toSun.xy * toSurface;
    let pattern = causticPattern(causticXy, U.params.z);
    // Fade with the depth of water above the lit point (shallow = crisp), and
    // never brighten a point deeper than the fog's far cap.
    let depthAbove = surfaceZ - world.z;
    let depthFade = clamp(1.0 - depthAbove / 12.0, 0.0, 1.0);
    let reach = transmit;  // caustic light is absorbed on the way to the eye too
    colour += U.caustic.rgb * pattern * U.caustic.w * depthFade * reach;
  }
  return vec4<f32>(colour, 1.0);
}
