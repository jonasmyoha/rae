// treeGen's leaf cards (docs/reference-tree-plan.md step 5; LeafPass.rae,
// LeafCards.rae): every card is one instance of a 6-vertex square that faces
// the camera, with the leaf picture on it. The picture's see-through gaps are
// cut out (discard) and the rest WRITES DEPTH into the G-buffer, so a leaf
// mass of cards breaks up into separate leaves that the trunk, the shadows
// and the AO all see. The shading normal is the card's mass's ellipsoid
// normal (lit top, dark underside, per mass), tilted a little across the
// leaflets so neighbouring leaves differ. Written new.
struct Frame {
  viewProj: mat4x4<f32>,
  prevViewProj: mat4x4<f32>,
  jitter: vec4<f32>,    // xy the TAA jitter
};
// One card: where and how big, its shading normal and occlusion, its green
// and the turn of the picture, and how much light comes through it
struct Card {
  positionSize: vec4<f32>,
  normalOcclusion: vec4<f32>,
  colourTurn: vec4<f32>,
  extra: vec4<f32>,     // x: the leaf stamp (image layer); w: light through from behind (0..1)
};
// The camera's right and up (world), where it is, and the direction TO the sun
struct LeafU {
  right: vec4<f32>,
  up: vec4<f32>,
  eye: vec4<f32>,
  toSun: vec4<f32>,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> cards: array<Card>;
@group(0) @binding(2) var<uniform> U: LeafU;
@group(0) @binding(3) var leafImage: texture_2d_array<f32>;
@group(0) @binding(4) var leafSampler: sampler;

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
  @location(0) @interpolate(flat) card: u32,
  @location(1) uv: vec2<f32>,
  @location(2) clipNow: vec4<f32>,
  @location(3) clipPrev: vec4<f32>,
  @location(4) wpos: vec3<f32>,
};

fn cornerOf(vertex: u32) -> vec2<f32> {
  let corners = array<vec2<f32>, 6>(vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0),
    vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0), vec2<f32>(0.0, 1.0));
  return corners[vertex % 6u];
}

@vertex
fn vs(@builtin(instance_index) ii: u32, @builtin(vertex_index) vi: u32) -> VsOut {
  let card = cards[ii];
  let corner = cornerOf(vi);
  // the square, turned in the camera's plane by the card's own turn
  let local = (corner - vec2<f32>(0.5)) * card.positionSize.w;
  let turn = card.colourTurn.w;
  let c = cos(turn);
  let s = sin(turn);
  let across = local.x * c - local.y * s;
  let upward = local.x * s + local.y * c;
  let world = card.positionSize.xyz + U.right.xyz * across + U.up.xyz * upward;
  var o: VsOut;
  o.pos = F.viewProj * vec4<f32>(world, 1.0);
  o.clipNow = o.pos;
  o.clipPrev = F.prevViewProj * vec4<f32>(world, 1.0);
  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);
  o.card = ii;
  o.uv = vec2<f32>(corner.x, 1.0 - corner.y);
  o.wpos = world;
  return o;
}

struct FsOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
};

@fragment
fn fs(in: VsOut) -> FsOut {
  let card = cards[in.card];
  let picture = textureSample(leafImage, leafSampler, in.uv, i32(card.extra.x + 0.5));
  if (picture.a < 0.5) { discard; }
  // the mass's normal, tilted a little toward where on the card this is, so
  // the leaves on one card do not all shade alike
  let massNormal = normalize(card.normalOcclusion.xyz);
  let toEye = normalize(U.eye.xyz - in.wpos);
  let offset = (in.uv - vec2<f32>(0.5)) * 2.0;
  var n = normalize(massNormal + (U.right.xyz * offset.x + U.up.xyz * -offset.y) * 0.28);
  // a card on the mass's far side shows its face to the camera: turn the
  // part of its normal that points away back toward the viewer, keeping its
  // sideways and up/down lean (otherwise the light model sees the leaf edge
  // on and paints it with a pale sky reflection)
  let away = dot(n, toEye);
  if (away < 0.15) { n = normalize(n + toEye * (0.15 - away)); }
  var albedo = card.colourTurn.rgb * picture.rgb;
  // light through the leaves: looking toward the sun at a card on the mass's
  // far side (its normal away from the sun) brightens and warms it a little
  let backLit = max(dot(-toEye, U.toSun.xyz), 0.0) * max(-dot(massNormal, U.toSun.xyz), 0.0);
  albedo = albedo * (1.0 + 0.45 * backLit * card.extra.w * vec3<f32>(1.15, 1.1, 0.7));
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  let oct = octEncode(n);
  var o: FsOut;
  // z 1: TAA-reactive, as thin leaves ghost on the history like grass blades;
  // w the material occlusion (deeper in the mass and lower in the tree)
  o.gba = vec4<f32>(oct.x, oct.y, 1.0, clamp(card.normalOcclusion.w, 0.0, 1.0));
  o.gbb = vec4<f32>(albedo, 0.8);
  // metallic 0; shading mode 0: lit
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, 0.0, 0.0);
  return o;
}
