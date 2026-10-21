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
//
// IMPOSTOR mode (SpriteU b.z = views per side > 0, lib/ImpostorAtlas,
// docs/tree-lod-design.md §2.2): the array is an octahedral impostor atlas
// (impostor_octahedral.wgsl) and the layer's frame (binding 6) places the
// prop's bounding sphere. The quad is the sphere's square, facing the eye.
// The direction to the eye, turned into the prop's own frame by its yaw (the
// model's turn about z), picks the three nearest baked views and their
// weights (b.w = 1; else the nearest view alone, the phone setting). Each
// view is read where the eye's ray meets the prop's surface (the baked depth
// as a parallax step) and the three are blended. The fragment writes the
// baked albedo, normal (turned to the world) and the depth of the point it
// shows into the G-buffer, so the sun, moon, shadows and SSAO light it like
// the mesh it stands in for. (The forward pass keeps the quad's depth.)

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
  // alpha test's band round the cutoff (0: a plain cut). b.z = the impostor
  // grid's views per side (0: plain pictures), b.w = 1 to blend three views.
  a: vec4<f32>,
  b: vec4<f32>,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> draws: array<DrawU>;
@group(0) @binding(2) var<uniform> S: SpriteU;
@group(0) @binding(3) var spriteTex: texture_2d_array<f32>;
@group(0) @binding(4) var spriteSamp: sampler;
// Impostor mode: the atlas's normal and depth, and per layer the bounding
// sphere's centre and radius over the prop's height (the plain mode binds
// the sprite array and an unread buffer here)
@group(0) @binding(5) var impostorNormalDepth: texture_2d_array<f32>;
@group(0) @binding(6) var<storage, read> impostorFrames: array<vec4<f32>>;

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
  // Impostor mode (unused otherwise): where the fragment falls on the
  // pictures of the three views (-1..1 along each view's right and up),
  // their grid cells and weights, the eye's direction in the prop's frame
  // with the prop's yaw, and the sphere's centre and radius
  @location(8) localAB: vec4<f32>,
  @location(9) localC: vec2<f32>,
  @location(10) @interpolate(flat) cellsAB: vec4<f32>,
  @location(11) @interpolate(flat) cellCWeights: vec4<f32>,
  @location(12) @interpolate(flat) eyeYaw: vec4<f32>,
  @location(13) @interpolate(flat) centreRadius: vec4<f32>,
};

// The eye's world position, from the view-projection: the point that clip
// x, y and w are all zero at (rows 0, 1 and 3; Cramer's rule)
fn cameraPosition() -> vec3<f32> {
  let m = F.viewProj;
  let r0 = vec4<f32>(m[0].x, m[1].x, m[2].x, m[3].x);
  let r1 = vec4<f32>(m[0].y, m[1].y, m[2].y, m[3].y);
  let r3 = vec4<f32>(m[0].w, m[1].w, m[2].w, m[3].w);
  let bc = cross(r1.xyz, r3.xyz);
  let det = dot(r0.xyz, bc);
  let rhs = -vec3<f32>(r0.w, r1.w, r3.w);
  return (rhs.x * bc + rhs.y * cross(r3.xyz, r0.xyz) + rhs.z * cross(r0.xyz, r1.xyz)) / det;
}

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
  var world = vec3<f32>(0.0);
  o.localAB = vec4<f32>(0.0);
  o.localC = vec2<f32>(0.0);
  o.cellsAB = vec4<f32>(0.0);
  o.cellCWeights = vec4<f32>(0.0);
  o.eyeYaw = vec4<f32>(0.0);
  o.centreRadius = vec4<f32>(0.0);
  let views = S.b.z;
  if (views > 0.5) {
    // The impostor: the sphere's square facing the eye
    let yaw = atan2(d.model[0].y, d.model[0].x);
    let frame = impostorFrames[u32(max(d.params.w, 0.0))];
    let centre = base + impostorTurn(frame.xyz, yaw) * height;
    let radius = frame.w * height;
    var toEye = cameraPosition() - centre;
    if (length(toEye) < 0.0001) {
      toEye = vec3<f32>(1.0, 0.0, 0.0);
    }
    let eyeWorld = normalize(toEye);
    let quadRight = impostorRight(eyeWorld);
    let quadUp = cross(eyeWorld, quadRight);
    let local = c * 2.0 - vec2<f32>(1.0);
    let offset = quadRight * local.x + quadUp * local.y;
    world = centre + offset * radius;
    // The views, in the prop's own frame
    let eye = impostorUnturn(eyeWorld, yaw);
    let point = impostorUnturn(offset, yaw);
    var picks: ImpostorViews;
    if (S.b.w > 0.5) {
      picks = impostorThreeViews(eye, views);
    } else {
      picks.a = impostorNearestView(eye, views);
      picks.b = picks.a;
      picks.c = picks.a;
      picks.weights = vec3<f32>(1.0, 0.0, 0.0);
    }
    let localA = impostorViewPoint(point, eye, impostorViewDirection(picks.a, views));
    o.localAB = vec4<f32>(localA, impostorViewPoint(point, eye, impostorViewDirection(picks.b, views)));
    o.localC = impostorViewPoint(point, eye, impostorViewDirection(picks.c, views));
    o.cellsAB = vec4<f32>(picks.a, picks.b);
    o.cellCWeights = vec4<f32>(picks.c, picks.weights.y, picks.weights.z);
    o.eyeYaw = vec4<f32>(eye, yaw);
    o.centreRadius = vec4<f32>(centre, radius);
    // The first view's atlas point: what the hashed alpha test measures
    o.uv = impostorAtlasUv(picks.a, views, localA);
  } else {
    let u = c.x - 0.5;               // -0.5..0.5 across width
    world = base + rightH * (u * width) + up * (c.y * height);
    // Sprite image has the crown at the TOP (row 0) and trunk at the BOTTOM, so the
    // quad's top (c.y=1) samples image v=0.
    o.uv = vec2<f32>(c.x, 1.0 - c.y);
  }
  o.pos = F.viewProj * vec4<f32>(world, 1.0);
  o.clipNow = o.pos;
  o.clipPrev = F.prevViewProj * vec4<f32>(world, 1.0);   // static prop: same world
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
// The depth the G-buffer entry writes: the quad's, or the point an
// impostor shows (set by gbufferSurface)
var<private> surfaceDepth: f32;

// One view of an impostor where the eye's ray meets the prop: its albedo
// (with alpha), normal (in the prop's frame) and that point (in the prop's
// frame, over the radius)
struct ImpostorTap {
  albedo: vec4<f32>,
  normal: vec3<f32>,
  position: vec3<f32>,
};
fn impostorTap(cell: vec2<f32>, local: vec2<f32>, planeAlpha: f32, eye: vec3<f32>, layer: i32) -> ImpostorTap {
  let views = S.b.z;
  let viewDirection = impostorViewDirection(cell, views);
  // One parallax step: the depth where the ray crosses the view's plane
  // (none where nothing was baked) moves the read to where it meets the prop
  let planeUv = impostorAtlasUv(cell, views, local);
  let planeDepth = textureSample(impostorNormalDepth, spriteSamp, planeUv, layer).a * 2.0 - 1.0;
  let moved = impostorParallax(local, planeDepth * planeAlpha, eye, viewDirection);
  let uv = impostorAtlasUv(cell, views, moved);
  let normalDepth = textureSample(impostorNormalDepth, spriteSamp, uv, layer);
  var tap: ImpostorTap;
  tap.albedo = textureSample(spriteTex, spriteSamp, uv, layer);
  tap.normal = normalDepth.xyz * 2.0 - vec3<f32>(1.0);
  tap.position = impostorViewPosition(moved, normalDepth.a * 2.0 - 1.0, viewDirection);
  return tap;
}

// A view's alpha where the eye's ray crosses its plane
fn impostorPlaneAlpha(cell: vec2<f32>, local: vec2<f32>, layer: i32) -> f32 {
  return textureSample(spriteTex, spriteSamp, impostorAtlasUv(cell, S.b.z, local), layer).a;
}

// An impostor's surface: its views blended by weight and alpha
fn impostorSurface(in: VsOut) -> FsOut {
  let eye = in.eyeYaw.xyz;
  let yaw = in.eyeYaw.w;
  let blend = S.b.w > 0.5;
  // Where every view is empty at its plane, there is no parallax either, so
  // the fragment is empty: most of the square (its corners, the gaps in a
  // crown) leaves here after one read a view, before the full reads (the
  // depth this pass writes turns off the early depth test, so a hidden
  // impostor's fragments run too)
  let alphaA = impostorPlaneAlpha(in.cellsAB.xy, in.localAB.xy, in.layer);
  var alphaB = 0.0;
  var alphaC = 0.0;
  if (blend) {
    alphaB = impostorPlaneAlpha(in.cellsAB.zw, in.localAB.zw, in.layer);
    alphaC = impostorPlaneAlpha(in.cellCWeights.xy, in.localC, in.layer);
  }
  if (max(alphaA, max(alphaB, alphaC)) <= 0.0) {
    discard;
  }
  let first = impostorTap(in.cellsAB.xy, in.localAB.xy, alphaA, eye, in.layer);
  var weights = vec3<f32>(1.0, 0.0, 0.0);
  var second = first;
  var third = first;
  if (blend) {
    weights = vec3<f32>(1.0 - in.cellCWeights.z - in.cellCWeights.w, in.cellCWeights.z, in.cellCWeights.w);
    second = impostorTap(in.cellsAB.zw, in.localAB.zw, alphaB, eye, in.layer);
    third = impostorTap(in.cellCWeights.xy, in.localC, alphaC, eye, in.layer);
  }
  let alpha = dot(weights, vec3<f32>(first.albedo.a, second.albedo.a, third.albedo.a));
  // alpha cutout — no colour, no depth; and a level fading in or out
  if (alpha < alphaThreshold(in) || !fadeKeeps(in.fade, in.pos.xy, fadeFrameOffset(F.jitter.xy))) {
    discard;
  }
  // Each view counts by its weight and by how much of the prop it holds here
  let share = weights * vec3<f32>(first.albedo.a, second.albedo.a, third.albedo.a);
  let total = max(share.x + share.y + share.z, 0.0001);
  let albedo = (first.albedo.rgb * share.x + second.albedo.rgb * share.y + third.albedo.rgb * share.z) / total;
  let normal = first.normal * share.x + second.normal * share.y + third.normal * share.z;
  let position = (first.position * share.x + second.position * share.y + third.position * share.z) / total;
  let worldNormal = impostorTurn(normal / max(length(normal), 0.0001), yaw);
  let world = in.centreRadius.xyz + impostorTurn(position, yaw) * in.centreRadius.w;
  let clip = F.viewProj * vec4<f32>(world, 1.0);
  surfaceDepth = clamp(clip.z / clip.w, 0.0, 1.0);
  let oct = octEncode(worldNormal);
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  var o: FsOut;
  o.gba = vec4<f32>(oct.x, oct.y, 0.5, in.mode);
  // The foliage the impostors stand for is fully rough (GroveTrees)
  o.gbb = vec4<f32>(albedo, 1.0);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, 0.0, in.emissive);
  return o;
}

// The surface at this fragment, in the G-buffer encoding: the G-buffer entry
// below writes it, the forward main pass's fsForward (forward_entry.wgsl)
// decodes and shades it. The cutout discards in both.
fn gbufferSurface(in: VsOut) -> FsOut {
  surfaceDepth = in.pos.z;
  if (S.b.z > 0.5) {
    return impostorSurface(in);
  }
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

// The G-buffer entry: the surface, and its depth (an impostor's moved one)
struct GbufferOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
  @builtin(frag_depth) depth: f32,
};
@fragment
fn fs(in: VsOut) -> GbufferOut {
  let surface = gbufferSurface(in);
  var o: GbufferOut;
  o.gba = surface.gba;
  o.gbb = surface.gbb;
  o.gbc = surface.gbc;
  o.depth = surfaceDepth;
  return o;
}

// The same with the quad's depth: the early depth test stays on (far props)
@fragment
fn fsQuadDepth(in: VsOut) -> FsOut {
  return gbufferSurface(in);
}
