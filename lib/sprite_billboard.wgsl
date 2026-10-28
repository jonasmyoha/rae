// Billboard SPRITE pass (#15). Generalizes the grass render pass — a procedural
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
// writes no color AND no depth there, giving a clean edge with correct occlusion.
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
  // c.x = 1: march each view (a box framed atlas, the far canopy's clusters:
  // impostorMarchSurface)
  c: vec4<f32>,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> draws: array<DrawU>;
@group(0) @binding(2) var<uniform> S: SpriteU;
@group(0) @binding(3) var spriteTex: texture_2d_array<f32>;
@group(0) @binding(4) var spriteSamp: sampler;
// Impostor mode: the atlas's normal and depth, and per subject (the
// instance's "layer") three vec4: the bounding sphere's center and radius
// over the prop's height, its page (texture layer, origin u, v, size) and
// its framing (impostorExtents: a box's half sides over the radius and 1, or
// 0 for a sphere) (the plain mode binds the sprite array and an unread
// buffer here)
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
  // with the prop's yaw, and the sphere's center and radius
  @location(8) localAB: vec4<f32>,
  @location(9) localC: vec2<f32>,
  @location(10) @interpolate(flat) cellsAB: vec4<f32>,
  @location(11) @interpolate(flat) cellCWeights: vec4<f32>,
  @location(12) @interpolate(flat) eyeYaw: vec4<f32>,
  @location(13) @interpolate(flat) centerRadius: vec4<f32>,
  // The subject's page in its texture layer (origin u, v, size) and the
  // subject (its framing is read from the frames)
  @location(14) @interpolate(flat) page: vec4<f32>,
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
  o.centerRadius = vec4<f32>(0.0);
  o.page = vec4<f32>(0.0, 0.0, 1.0, 0.0);
  o.layer = i32(max(d.params.w, 0.0));
  let views = S.b.z;
  if (views > 0.5) {
    // The impostor: the sphere's square facing the eye
    let yaw = atan2(d.model[0].y, d.model[0].x);
    let subject = u32(max(d.params.w, 0.0));
    let frame = impostorFrames[subject * 3u];
    let pageOf = impostorFrames[subject * 3u + 1u];
    let box = impostorFrames[subject * 3u + 2u];
    o.layer = i32(pageOf.x);
    o.page = vec4<f32>(pageOf.yzw, f32(subject));
    let center = base + impostorTurn(frame.xyz, yaw) * height;
    let radius = frame.w * height;
    var toEye = cameraPosition() - center;
    if (length(toEye) < 0.0001) {
      toEye = vec3<f32>(1.0, 0.0, 0.0);
    }
    let eyeWorld = normalize(toEye);
    let quadRight = impostorRight(eyeWorld);
    let quadUp = cross(eyeWorld, quadRight);
    let local = c * 2.0 - vec2<f32>(1.0);
    // A box framed subject's quad stands at the box's front toward the eye
    // (its depth, the quad's, is then never behind its trees: a cluster on a
    // slope is not hidden by the ground in its middle); the views are read
    // along the same rays, so the picture is the same
    // and is as wide and tall as the box's outline (a wide, low cluster
    // leaves most of the sphere's square empty)
    var forward = 0.0;
    var reach = vec2<f32>(1.0);
    if (box.w > 0.5) {
      forward = dot(abs(impostorUnturn(eyeWorld, yaw)), box.xyz);
      reach = vec2<f32>(
        dot(abs(impostorUnturn(quadRight, yaw)), box.xyz), dot(abs(impostorUnturn(quadUp, yaw)), box.xyz));
    }
    let offset = quadRight * (local.x * reach.x) + quadUp * (local.y * reach.y) + eyeWorld * forward;
    world = center + offset * radius;
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
    let directionA = impostorViewDirection(picks.a, views);
    let directionB = impostorViewDirection(picks.b, views);
    let directionC = impostorViewDirection(picks.c, views);
    let localA = impostorViewPoint(point, eye, directionA, impostorExtents(directionA, box));
    let localB = impostorViewPoint(point, eye, directionB, impostorExtents(directionB, box));
    o.localAB = vec4<f32>(localA, localB);
    o.localC = impostorViewPoint(point, eye, directionC, impostorExtents(directionC, box));
    if (S.c.x > 0.5) {
      // Marched: the quad's point in the subject's frame, over its radius
      o.localAB = vec4<f32>(point, 0.0);
    }
    o.cellsAB = vec4<f32>(picks.a, picks.b);
    o.cellCWeights = vec4<f32>(picks.c, picks.weights.y, picks.weights.z);
    o.eyeYaw = vec4<f32>(eye, yaw);
    o.centerRadius = vec4<f32>(center, radius);
    // The first view's atlas point: what the hashed alpha test measures
    o.uv = impostorPageUv(o.page.xyz, impostorAtlasUv(picks.a, views, localA));
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
  // emissive; a negative one marks a prop without the sky's rim light
  // (its gba.z below 0.25, as gbuffer_static.wgsl marks a mesh's)
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
// (app3d/SpriteBake spriteMipChain), and a band centered on the cutoff keeps
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
fn impostorTap(
  cell: vec2<f32>, local: vec2<f32>, planeAlpha: f32, eye: vec3<f32>, layer: i32, page: vec4<f32>,
) -> ImpostorTap {
  let views = S.b.z;
  let viewDirection = impostorViewDirection(cell, views);
  let extents = impostorExtents(viewDirection, impostorFrames[u32(page.w) * 3u + 2u]);
  // One parallax step: the depth where the ray crosses the view's plane
  // (none where nothing was baked) moves the read to where it meets the prop
  let planeUv = impostorPageUv(page.xyz, impostorAtlasUv(cell, views, local));
  let planeDepth = impostorDepthOf(textureSample(impostorNormalDepth, spriteSamp, planeUv, layer), planeAlpha);
  let moved = impostorParallax(local, planeDepth * min(planeAlpha * 2.0, 1.0), eye, viewDirection, extents);
  let uv = impostorPageUv(page.xyz, impostorAtlasUv(cell, views, moved));
  let normalDepth = textureSample(impostorNormalDepth, spriteSamp, uv, layer);
  var tap: ImpostorTap;
  tap.albedo = textureSample(spriteTex, spriteSamp, uv, layer);
  // Off the view's picture (a box framed view is smaller than the quad):
  // nothing there, not the next view's picture
  tap.albedo.a = tap.albedo.a * impostorOnPicture(local);
  tap.normal = impostorUnpackNormal(normalDepth.xy);
  tap.position = impostorViewPosition(moved, impostorDepthOf(normalDepth, tap.albedo.a), viewDirection, extents);
  return tap;
}

// A view's alpha where the eye's ray crosses its plane
fn impostorPlaneAlpha(cell: vec2<f32>, local: vec2<f32>, layer: i32, page: vec4<f32>) -> f32 {
  let uv = impostorPageUv(page.xyz, impostorAtlasUv(cell, S.b.z, local));
  return textureSample(spriteTex, spriteSamp, uv, layer).a * impostorOnPicture(local);
}

// 1 where `local` is on a view's picture (-1..1 both ways), else 0
fn impostorOnPicture(local: vec2<f32>) -> f32 {
  return select(0.0, 1.0, abs(local.x) <= 1.0 && abs(local.y) <= 1.0);
}

// One view of a box framed subject (a canopy cluster: deep and sparse, its
// views far apart) marched along the eye's ray: from where the ray enters
// the box to where it leaves, each step's point projected onto the view's
// picture, the first that lies within a crown's thickness behind the
// surface the view baked there (the depth the bake stored there: about a
// crown, almost nothing at a trunk). The view's trees are found where they are,
// not where the view drew them, so a cluster seen between two views keeps
// its trees. `albedo.a` is 1 on a hit, else 0.
const impostorMarchSteps: u32 = 12u;
// Halvings that find where the ray enters the prop between two steps
const impostorMarchHalvings: u32 = 3u;

// What a view baked at atlas point `uv`, for a point of the ray at depth
// `depth` (toward the view's eye, depth units): how far the point is behind
// the surface there, and how deep the prop is there (both impostorEmpty
// where nothing is)
const impostorEmpty: f32 = -1000.0;
fn impostorBehind(uv: vec2<f32>, depth: f32, layer: i32, lod: f32, threshold: f32) -> vec2<f32> {
  let alpha = textureSampleLevel(spriteTex, spriteSamp, uv, layer, lod).a;
  if (alpha < threshold) {
    return vec2<f32>(impostorEmpty);
  }
  let normalDepth = textureSampleLevel(impostorNormalDepth, spriteSamp, uv, layer, lod);
  return vec2<f32>(impostorDepthOf(normalDepth, alpha) - depth, impostorThicknessOf(normalDepth, alpha));
}

fn impostorMarch(
  cell: vec2<f32>, point: vec3<f32>, eye: vec3<f32>, box: vec4<f32>, layer: i32, page: vec4<f32>,
  lod: f32, threshold: f32,
) -> ImpostorTap {
  let views = S.b.z;
  let viewDirection = impostorViewDirection(cell, views);
  let extents = impostorExtents(viewDirection, box);
  let right = impostorRight(viewDirection);
  let up = impostorUp(viewDirection);
  var tap: ImpostorTap;
  tap.albedo = vec4<f32>(0.0);
  tap.normal = vec3<f32>(0.0, 0.0, 1.0);
  tap.position = point;
  // The ray point - eye * t inside the box (|x| <= box.xyz)
  let away = -eye;
  let safe = select(away, vec3<f32>(0.000001), abs(away) < vec3<f32>(0.000001));
  let first = (-box.xyz - point) / safe;
  let second = (box.xyz - point) / safe;
  let enter = max(max(min(first.x, second.x), min(first.y, second.y)), min(first.z, second.z));
  let leave = min(min(max(first.x, second.x), max(first.y, second.y)), max(first.z, second.z));
  if (leave <= enter) {
    return tap;
  }
  // As many steps as the ray crosses texels of the view's picture at the
  // level it is read at (a far cluster's picture is a few texels across),
  // at least 4 and at most impostorMarchSteps
  let entering = point + away * enter;
  let leaving = point + away * leave;
  let across = vec2<f32>(dot(leaving - entering, right) / extents.x, dot(leaving - entering, up) / extents.y);
  let cellTexels = vec2<f32>(textureDimensions(spriteTex, 0)) * page.z / views / exp2(lod);
  let texelsCrossed = length(across * 0.5 * cellTexels);
  let steps = clamp(u32(ceil(texelsCrossed)) + 2u, 4u, impostorMarchSteps);
  // Nothing on the ray at all: four reads of a level two coarser (a coarse
  // texel averages the finer ones, so it is 0 only where all are empty, and
  // filtered it reaches past its texel) skip the march for the empty parts
  // of the quad, most of it where clusters overlap low across a forest
  var anything = 0.0;
  for (var probe = 0u; probe < 4u; probe = probe + 1u) {
    let spot = point + away * mix(enter, leave, (f32(probe) + 0.5) / 4.0);
    let local = vec2<f32>(dot(spot, right) / extents.x, dot(spot, up) / extents.y);
    let uv = impostorPageUv(page.xyz, impostorAtlasUv(cell, views, local));
    anything = max(anything, textureSampleLevel(spriteTex, spriteSamp, uv, layer, lod + 2.0).a);
  }
  if (anything <= 0.0) {
    return tap;
  }
  // The ray's atlas point and depth are linear in t: worked out once
  let uvAt = impostorPageUv(page.xyz, impostorAtlasUv(cell, views, vec2<f32>(dot(point, right) / extents.x, dot(point, up) / extents.y)));
  let uvMoved = impostorPageUv(page.xyz, impostorAtlasUv(cell, views, vec2<f32>(dot(point + away, right) / extents.x, dot(point + away, up) / extents.y)));
  let uvPerT = uvMoved - uvAt;
  let depthAt = dot(point, viewDirection) / extents.z;
  let depthPerT = dot(away, viewDirection) / extents.z;
  // From the front, the first step that is behind the surface baked where
  // it falls. In the prop (within the depth baked there): a hit. Behind it:
  // halve back to where the ray crosses into the prop's picture or through
  // its surface, and keep that if it lies within the depth baked there;
  // else go on past (behind that prop, not in it)
  var front = enter;
  var hit = -1000.0;
  for (var marched = 0u; marched < steps; marched = marched + 1u) {
    let t = mix(enter, leave, (f32(marched) + 1.0) / f32(steps));
    var inside = impostorBehind(uvAt + uvPerT * t, depthAt + depthPerT * t, layer, lod, threshold);
    if (inside.x < 0.0) {
      front = t;
      continue;
    }
    // Already in the prop: taken as it is (no halving needed to say so)
    if (inside.x <= inside.y + 0.02) {
      hit = t;
      break;
    }
    var low = front;
    var high = t;
    for (var halving = 0u; halving < impostorMarchHalvings; halving = halving + 1u) {
      let middle = (low + high) * 0.5;
      let probe = impostorBehind(uvAt + uvPerT * middle, depthAt + depthPerT * middle, layer, lod, threshold);
      if (probe.x < 0.0) {
        low = middle;
      } else {
        high = middle;
        inside = probe;
      }
    }
    // How far into the prop the ray may already be at `high`: the depth
    // the interval left after the halvings spans
    let unsure = (high - low) * abs(depthPerT);
    if (inside.x <= inside.y + unsure + 0.02) {
      hit = high;
      break;
    }
    front = t;
  }
  if (hit > -1000.0) {
    let spot = point + away * hit;
    let local = vec2<f32>(dot(spot, right) / extents.x, dot(spot, up) / extents.y);
    let uv = impostorPageUv(page.xyz, impostorAtlasUv(cell, views, local));
    tap.albedo = vec4<f32>(textureSampleLevel(spriteTex, spriteSamp, uv, layer, lod).rgb, 1.0);
    tap.normal = impostorUnpackNormal(textureSampleLevel(impostorNormalDepth, spriteSamp, uv, layer, lod).xy);
    tap.position = spot;
  }
  return tap;
}

// A marched subject's surface: the prop's where the view marched finds it
// on the eye's ray (marched, the views agree on where the trees are, so a
// vote of three changes almost nothing; checks/canopyCoverage)
fn impostorMarchSurface(in: VsOut) -> FsOut {
  let yaw = in.eyeYaw.w;
  let point = in.localAB.xyz;
  let box = impostorFrames[u32(in.page.w) * 3u + 2u];
  let radius = in.centerRadius.w;
  // This fragment's own ray from the eye (the quad stands at the box's
  // front, so the one direction of the quad's center is off at its edges)
  let pointWorld = in.centerRadius.xyz + impostorTurn(point, yaw) * radius;
  let eye = impostorUnturn(normalize(cameraPosition() - pointWorld), yaw);
  // The picture's mip level, from how fast the first view's picture moves
  // across the screen (the march reads at one level)
  let texels = in.uv * vec2<f32>(textureDimensions(spriteTex, 0));
  let footprint = max(length(dpdx(texels)), length(dpdy(texels)));
  let lod = max(log2(max(footprint, 1.0)), 0.0);
  let threshold = alphaThreshold(in);
  // One view marched a fragment: with three, each by its weight, by a
  // dither over the screen (stable without TAA, resolved by it), so the
  // picture moves from view to view without a pop at a third of the cost
  var cell = in.cellsAB.xy;
  if (S.b.w > 0.5) {
    let pick = fadeDither(in.pos.xy, 0.731, fadeFrameOffset(F.jitter.xy));
    let first = 1.0 - in.cellCWeights.z - in.cellCWeights.w;
    if (pick >= first) {
      cell = in.cellsAB.zw;
      if (pick >= first + in.cellCWeights.z) {
        cell = in.cellCWeights.xy;
      }
    }
  }
  let tap = impostorMarch(cell, point, eye, box, in.layer, in.page, lod, threshold);
  if (tap.albedo.a < 0.5 || !fadeKeeps(in.fade, in.pos.xy, fadeFrameOffset(F.jitter.xy))) {
    discard;
  }
  let albedo = tap.albedo.rgb;
  let normal = tap.normal;
  let position = tap.position;
  let worldNormal = impostorTurn(normal / max(length(normal), 0.0001), yaw);
  let world = in.centerRadius.xyz + impostorTurn(position, yaw) * radius;
  let clip = F.viewProj * vec4<f32>(world, 1.0);
  surfaceDepth = clamp(clip.z / clip.w, 0.0, 1.0);
  let oct = octEncode(worldNormal);
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  var o: FsOut;
  o.gba = vec4<f32>(oct.x, oct.y, select(0.5, 0.1, in.emissive < 0.0), in.mode);
  o.gbb = vec4<f32>(albedo, 1.0);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, 0.0, max(in.emissive, 0.0));
  return o;
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
  let alphaA = impostorPlaneAlpha(in.cellsAB.xy, in.localAB.xy, in.layer, in.page);
  var alphaB = 0.0;
  var alphaC = 0.0;
  if (blend) {
    alphaB = impostorPlaneAlpha(in.cellsAB.zw, in.localAB.zw, in.layer, in.page);
    alphaC = impostorPlaneAlpha(in.cellCWeights.xy, in.localC, in.layer, in.page);
  }
  if (max(alphaA, max(alphaB, alphaC)) <= 0.0) {
    discard;
  }
  let first = impostorTap(in.cellsAB.xy, in.localAB.xy, alphaA, eye, in.layer, in.page);
  var weights = vec3<f32>(1.0, 0.0, 0.0);
  var second = first;
  var third = first;
  if (blend) {
    weights = vec3<f32>(1.0 - in.cellCWeights.z - in.cellCWeights.w, in.cellCWeights.z, in.cellCWeights.w);
    second = impostorTap(in.cellsAB.zw, in.localAB.zw, alphaB, eye, in.layer, in.page);
    third = impostorTap(in.cellCWeights.xy, in.localC, alphaC, eye, in.layer, in.page);
  }
  let alpha = dot(weights, vec3<f32>(first.albedo.a, second.albedo.a, third.albedo.a));
  // alpha cutout — no color, no depth; and a level fading in or out
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
  let world = in.centerRadius.xyz + impostorTurn(position, yaw) * in.centerRadius.w;
  let clip = F.viewProj * vec4<f32>(world, 1.0);
  surfaceDepth = clamp(clip.z / clip.w, 0.0, 1.0);
  let oct = octEncode(worldNormal);
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  var o: FsOut;
  o.gba = vec4<f32>(oct.x, oct.y, select(0.5, 0.1, in.emissive < 0.0), in.mode);
  // The foliage the impostors stand for is fully rough (GroveTrees)
  o.gbb = vec4<f32>(albedo, 1.0);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, 0.0, max(in.emissive, 0.0));
  return o;
}

// The surface at this fragment, in the G-buffer encoding: the G-buffer entry
// below writes it, the forward main pass's fsForward (forward_entry.wgsl)
// decodes and shades it. The cutout discards in both.
fn gbufferSurface(in: VsOut) -> FsOut {
  surfaceDepth = in.pos.z;
  if (S.c.x > 0.5) {
    return impostorMarchSurface(in);
  }
  if (S.b.z > 0.5) {
    return impostorSurface(in);
  }
  let texel = textureSample(spriteTex, spriteSamp, in.uv, in.layer);
  let threshold = alphaThreshold(in);
  // alpha cutout — no color, no depth; and a level fading in or out
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
  o.gba = vec4<f32>(oct.x, oct.y, select(0.5, 0.1, in.emissive < 0.0), in.mode);      // the instance's mode (0 = lit)
  o.gbb = vec4<f32>(texel.rgb, 0.9);                  // albedo, high roughness
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, 0.0, max(in.emissive, 0.0));
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
