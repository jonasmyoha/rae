// Billboard SPRITES as shadow casters (ShadowMapsSprite.rae). The same
// pictures the G-buffer sprite pass draws (sprite_billboard.wgsl, one layer of
// the sprite array per prop kind), but each quad is turned to face the SUN
// rather than the camera: it stands on world +Z and turns only about Z, so its
// width runs along the light view's horizontal right axis and the sun sees its
// whole picture. Depth only; a texel below the alpha cutoff is discarded, so
// the shadow has the picture's outline, not the quad's.
//
// The sun's right axis comes from the cascade matrix itself (row 0 of the
// light view-projection is the world gradient of the map's x), the same trick
// the G-buffer sprite takes the camera's from, so it cannot disagree with the
// cascade fit.
//
// Impostor mode (P.a.z = views per side > 0, lib/ImpostorAtlas): the array
// is an octahedral impostor atlas and the layer's frame (binding 5) places
// the prop's bounding sphere; the quad stands at the sphere's centre, turned
// to the sun about z, with the side view nearest the sun's direction.
struct SU { lightViewProj: mat4x4<f32> };
// a.x = picture aspect (width over height), a.y = alpha cutoff, a.z = the
// impostor grid's views per side (0: plain pictures)
struct SpriteShadowU { a: vec4<f32> };
// Per sprite, two vec4: (foot x, y, z, world height), (layer, fade threshold,
// fading in, fade seed) — the level cross-fade of fade_dither.wgsl
@group(0) @binding(0) var<uniform> S: SU;
@group(0) @binding(1) var<storage, read> sprites: array<vec4<f32>>;
@group(0) @binding(2) var<uniform> P: SpriteShadowU;
@group(0) @binding(3) var spriteTex: texture_2d_array<f32>;
@group(0) @binding(4) var spriteSamp: sampler;
// Impostor mode: per layer the bounding sphere's centre and radius over the
// prop's height (an unread buffer otherwise)
@group(0) @binding(5) var<storage, read> impostorFrames: array<vec4<f32>>;

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
  @location(1) @interpolate(flat) layer: i32,
  @location(2) @interpolate(flat) fade: vec4<f32>,
};

@vertex
fn vs(@builtin(instance_index) ii: u32, @builtin(vertex_index) vi: u32) -> VsOut {
  var corner = array<vec2<f32>, 6>(
    vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),
    vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0), vec2<f32>(0.0, 1.0));
  let c = corner[vi];
  let foot = sprites[ii * 2u];
  let extra = sprites[ii * 2u + 1u];
  let height = foot.w;
  let width = height * P.a.x;
  let rc = vec3<f32>(S.lightViewProj[0].x, S.lightViewProj[1].x, S.lightViewProj[2].x);
  var rightH = vec3<f32>(rc.x, rc.y, 0.0);
  if (length(rightH) < 0.000001) {
    rightH = vec3<f32>(1.0, 0.0, 0.0);
  }
  rightH = normalize(rightH);
  var world = foot.xyz + rightH * ((c.x - 0.5) * width) + vec3<f32>(0.0, 0.0, c.y * height);
  var uv = vec2<f32>(c.x, 1.0 - c.y);
  let views = P.a.z;
  if (views > 0.5) {
    let frame = impostorFrames[u32(max(extra.x, 0.0))];
    let centre = foot.xyz + frame.xyz * height;
    let radius = frame.w * height;
    // Depth grows away from the sun (row 2 is its world gradient)
    var towardSun = -vec3<f32>(S.lightViewProj[0].z, S.lightViewProj[1].z, 0.0);
    if (length(towardSun) < 0.000001) {
      towardSun = vec3<f32>(1.0, 0.0, 0.0);
    }
    let direction = normalize(towardSun);
    let cell = impostorNearestView(direction, views);
    let local = c * 2.0 - vec2<f32>(1.0);
    world = centre + impostorRight(direction) * (local.x * radius) + vec3<f32>(0.0, 0.0, local.y * radius);
    uv = impostorAtlasUv(cell, views, local);
  }
  var o: VsOut;
  o.pos = S.lightViewProj * vec4<f32>(world, 1.0);
  // The picture's top row (v = 0) is the prop's top
  o.uv = uv;
  o.layer = i32(max(extra.x, 0.0));
  o.fade = vec4<f32>(extra.y, extra.z, extra.w, 0.0);
  return o;
}

@fragment
fn fs(in: VsOut) {
  let texel = textureSampleLevel(spriteTex, spriteSamp, in.uv, in.layer, 0.0);
  if (texel.a < P.a.y || !fadeKeeps(in.fade, in.pos.xy, vec2<f32>(0.0))) {
    discard;
  }
}
