// Billboard SPRITE pass (#15). Generalises the grass render pass — a procedural
// quad, no vertex buffer, instanced from the shared DrawU records — into a
// TEXTURED, alpha-tested, CYLINDRICAL billboard for standing props (trees, ...).
//
// CYLINDRICAL, yaw-only by default: the quad stands on world +Z and rotates only
// about Z to face the camera's horizontal bearing (SpriteU right vector). The
// sprite is authored at the game's fixed camera PITCH — see
// docs/textured-billboards-design.md §4. A TILT knob (SpriteU b.x, #22) can lean
// the quad's up-axis toward the camera so it lays flatter toward the ground; the
// steep top-down camera would otherwise see an upright quad almost edge-on.
//
// ALPHA-TESTED, not blended: the deferred G-buffer has no transparency path, so a
// cutout is a `discard` on low alpha (like the SDF/shadow miss shaders) — it
// writes no colour AND no depth there, giving a clean edge with correct occlusion.
//
// Layer (which prop image) rides in DrawU slot 39 = params.w, the field the
// skinned path calls paletteBase. A billboard is NEVER skinned, so the slot is
// free here; documented at the writer (gbuffer_instanced addSpriteInstance) too.

struct Frame {
  viewProj: mat4x4<f32>,
  prevViewProj: mat4x4<f32>,
  jitter: vec4<f32>,
};
struct DrawU {
  model: mat4x4<f32>,
  prevModel: mat4x4<f32>,
  albedoMetallic: vec4<f32>,
  params: vec4<f32>,   // sprite: x=per-instance world height, y=emissive, z=mode, w=layer (slot 39)
  fade: vec4<f32>,     // the level cross-fade (GbufferRecord [40..43]); 0 when not fading
};
struct SpriteU {
  // a.y = sprite aspect (w/h), a.z = alpha cutoff, a.w = time. (a.x unused —
  // height is per-instance in DrawU params.x.) b.x = tilt, b.y = the hashed
  // alpha test's band round the cutoff (0: a plain cut).
  a: vec4<f32>,
  b: vec4<f32>,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> draws: array<DrawU>;
@group(0) @binding(2) var<uniform> S: SpriteU;
@group(0) @binding(3) var spriteTex: texture_2d_array<f32>;
@group(0) @binding(4) var spriteSamp: sampler;

fn octWrap(v: vec2<f32>) -> vec2<f32> {
  let s = vec2<f32>(select(-1.0, 1.0, v.x >= 0.0), select(-1.0, 1.0, v.y >= 0.0));
  return (vec2<f32>(1.0) - abs(v.yx)) * s;
}
fn octEncode(n: vec3<f32>) -> vec2<f32> {
  var p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
  if (n.z < 0.0) { p = octWrap(p); }
  return p * 0.5 + vec2<f32>(0.5);
}

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
  @location(1) @interpolate(flat) layer: i32,
  @location(2) clipNow: vec4<f32>,
  @location(3) clipPrev: vec4<f32>,
  @location(4) @interpolate(flat) emissive: f32,
  @location(5) @interpolate(flat) mode: f32,
  // A per-prop seed for the hashed alpha test, from where it stands
  @location(6) @interpolate(flat) seed: f32,
  // Its level cross-fade (fade_dither.wgsl)
  @location(7) @interpolate(flat) fade: vec4<f32>,
};

// vertex_index 0..5 -> two triangles of a unit quad, u01 in 0..1, v01 in 0..1.
fn quadCorner(vi: u32) -> vec2<f32> {
  var q = array<vec2<f32>, 6>(
    vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),
    vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0), vec2<f32>(0.0, 1.0));
  return q[vi];
}

@vertex
fn vs(@builtin(instance_index) ii: u32, @builtin(vertex_index) vi: u32) -> VsOut {
  let d = draws[ii];
  var o: VsOut;
  // Instance world scale from a model column length; the quad is sized in world
  // units so distant props shrink with perspective like the meshes they replace.
  let scale = length(vec3<f32>(d.model[0].x, d.model[0].y, d.model[0].z));
  // Per-instance world height rides in params.x (a sprite ignores roughness), so
  // each prop KIND sizes independently — trees, bushes and flowers share one
  // pipeline but not one size. Times the instance scale for the #4 per-prop jitter.
  let height = scale * d.params.x;
  let width = height * S.a.y;
  // The instance position is the prop's BASE on the ground (model translation).
  let base = vec3<f32>(d.model[3].x, d.model[3].y, d.model[3].z);
  // Camera horizontal RIGHT, derived from the view-projection: row 0 is the
  // world-space gradient of clip.x, i.e. the world direction that moves a vertex
  // toward screen-right. Its horizontal part is the cylindrical quad's width axis
  // -- no camera uniform needed, and it tracks the camera if it ever orbits.
  let rc = vec3<f32>(F.viewProj[0].x, F.viewProj[1].x, F.viewProj[2].x);
  let rightH = normalize(vec3<f32>(rc.x, rc.y, 0.0));
  // TILT toward the camera (S.b.x, 0..1). At 0 the quad stands on world +Z as a
  // pure cylindrical billboard; at 1 its up-axis is the camera's up, so the card
  // lies back to face the camera fully. Under the fixed steep top-down view an
  // upright quad is seen almost edge-on -- tilting it lays the sprite flatter,
  // toward the ground, so the top-down camera sees its face (#22). camUp is the
  // world direction of screen-up, read from viewProj row 1 like rightH from row 0.
  let uc = vec3<f32>(F.viewProj[0].y, F.viewProj[1].y, F.viewProj[2].y);
  let camUp = normalize(uc);
  let up = normalize(mix(vec3<f32>(0.0, 0.0, 1.0), camUp, clamp(S.b.x, 0.0, 1.0)));

  let c = quadCorner(vi);          // u01, v01
  let u = c.x - 0.5;               // -0.5..0.5 across width
  let world = base + rightH * (u * width) + up * (c.y * height);
  o.pos = F.viewProj * vec4<f32>(world, 1.0);
  o.clipNow = o.pos;
  o.clipPrev = F.prevViewProj * vec4<f32>(world, 1.0);   // static prop: same world
  // Sprite image has the crown at the TOP (row 0) and trunk at the BOTTOM, so the
  // quad's top (c.y=1) samples image v=0.
  o.uv = vec2<f32>(c.x, 1.0 - c.y);
  o.layer = i32(max(d.params.w, 0.0));
  o.emissive = d.params.y;
  // The shading mode (lit / emissive / toon) as the mesh instances carry it,
  // so a billboard standing in for a toon mesh is shaded toon too
  o.mode = d.params.z;
  o.seed = fract(sin(dot(base.xy, vec2<f32>(12.9898, 78.233))) * 43758.5453);
  o.fade = d.fade;
  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);
  return o;
}

struct FsOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
};

// The alpha test's threshold: the cutoff, or with a band (b.y > 0, set when
// TAA resolves it) a hashed threshold in that band round the cutoff (Wyman and
// McGuire, "Hashed alpha testing", 2017). The hash is fixed per texel of the
// mip level being sampled and per prop, so the pattern stays on the picture
// as the camera moves; TAA's jitter samples it at moving points and resolves
// it into soft leaf edges. The mips keep their coverage under a plain cut
// (app3d/SpriteBake spriteMipChain), and a band centred on the cutoff keeps
// close to that coverage.
fn spriteHash(p: vec3<f32>) -> f32 {
  var q = fract(p * vec3<f32>(0.1031, 0.1030, 0.0973));
  q = q + dot(q, q.yxz + vec3<f32>(33.33));
  return fract((q.x + q.y) * q.z);
}
fn alphaThreshold(in: VsOut) -> f32 {
  let texelPos = in.uv * vec2<f32>(textureDimensions(spriteTex, 0));
  let footprint = max(length(dpdx(texelPos)), length(dpdy(texelPos)));
  let band = S.b.y;
  if (band <= 0.0) { return S.a.z; }
  let level = max(floor(log2(max(footprint, 1.0))), 0.0);
  let cell = floor(texelPos / exp2(level));
  let h = spriteHash(vec3<f32>(cell, in.seed * 1024.0 + level));
  return S.a.z + (h - 0.5) * band;
}
// The surface at this fragment, in the G-buffer encoding: the G-buffer entry
// below writes it, the forward main pass's fsForward (forward_entry.wgsl)
// decodes and shades it. The cutout discards in both.
fn gbufferSurface(in: VsOut) -> FsOut {
  let texel = textureSample(spriteTex, spriteSamp, in.uv, in.layer);
  let threshold = alphaThreshold(in);
  // alpha cutout — no colour, no depth; and a level fading in or out
  if (texel.a < threshold || !fadeKeeps(in.fade, in.pos.xy, fadeFrameOffset(F.jitter.xy))) {
    discard;
  }
  // The sprite pixels are already lit (cut from the plate). Face the normal UP so
  // the deferred pass adds only the gentle ground-like ambient/sun rather than a
  // hard directional term that would fight the baked shading — the same reasoning
  // grass uses for its up-facing blades.
  let oct = octEncode(vec3<f32>(0.0, 0.0, 1.0));
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  var o: FsOut;
  o.gba = vec4<f32>(oct.x, oct.y, 0.5, in.mode);      // the instance's mode (0 = lit)
  o.gbb = vec4<f32>(texel.rgb, 0.9);                  // albedo, high roughness
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, 0.0, in.emissive);
  return o;
}
@fragment
fn fs(in: VsOut) -> FsOut {
  return gbufferSurface(in);
}
