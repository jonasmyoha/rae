// The impostor bake (lib/ImpostorBake.rae, docs/tree-lod-design.md §2.2):
// a prop's mesh parts drawn into every view cell of its atlas layer at once.
// Each part is one instanced draw of views x views instances; the instance
// is the view, and the vertex is placed in that view's cell of the layer
// (impostor_octahedral.wgsl). The bounding sphere fills each cell, so no
// triangle reaches a neighbour's cell. Two targets: the part's unlit albedo
// with alpha 1, and its normal and depth.
struct BakeU {
  // x = views per side
  a: vec4<f32>,
};
@group(0) @binding(0) var<uniform> U: BakeU;
// Per part (by instance / views^2): its albedo and the layer it bakes into
@group(0) @binding(1) var<storage, read> parts: array<vec4<f32>>;
// Per layer: the bounding sphere's centre (mesh units) and radius
@group(0) @binding(2) var<storage, read> frames: array<vec4<f32>>;

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) normal: vec3<f32>,
  @location(1) @interpolate(flat) albedo: vec3<f32>,
  @location(2) @interpolate(flat) direction: vec3<f32>,
  // How near the eye the point is: -1 at the sphere's back, 1 at its front
  @location(3) depth: f32,
};

@vertex
fn vs(@builtin(instance_index) ii: u32,
      @location(0) position: vec3<f32>, @location(1) normal: vec3<f32>) -> VsOut {
  let views = u32(U.a.x);
  let viewCount = views * views;
  let part = parts[ii / viewCount];
  let index = ii % viewCount;
  let cell = vec2<f32>(f32(index % views), f32(index / views));
  let frame = frames[u32(part.w)];
  let direction = impostorViewDirection(cell, f32(views));
  let local = (position - frame.xyz) / frame.w;
  let x = dot(local, impostorRight(direction));
  let y = dot(local, impostorUp(direction));
  let depth = dot(local, direction);
  let uv = impostorAtlasUv(cell, f32(views), vec2<f32>(x, y));
  var o: VsOut;
  // Nearer the eye is a smaller depth (cleared to 1, test Less)
  o.pos = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5 - 0.5 * depth, 1.0);
  o.normal = normal;
  o.albedo = part.xyz;
  o.direction = direction;
  o.depth = depth;
  return o;
}

struct FsOut {
  // The albedo, unlit, and alpha 1 where the prop is
  @location(0) albedo: vec4<f32>,
  // The normal (object space), turned to the eye (leaves are two-sided),
  // the depth behind it (its mirror about the sphere's centre: about the
  // crown's depth, nothing at the trunk) and the depth (0.5 + 0.5 * depth:
  // 1 at the sphere's front); impostorPackNormal's layout
  @location(1) normalDepth: vec4<f32>,
};

@fragment
fn fs(in: VsOut) -> FsOut {
  var n = normalize(in.normal);
  if (dot(n, in.direction) < 0.0) {
    n = -n;
  }
  var o: FsOut;
  o.albedo = vec4<f32>(in.albedo, 1.0);
  o.normalDepth = vec4<f32>(
    impostorPackNormal(n), clamp(in.depth, 0.0, 1.0), clamp(0.5 + 0.5 * in.depth, 0.0, 1.0));
  return o;
}
