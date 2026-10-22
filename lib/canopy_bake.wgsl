// The far canopy's bake (lib/CanopyBake.rae, docs/tree-lod-design.md §2.4):
// every tree of a cluster (a PropField cell's trees) drawn into each view
// cell of the cluster's page, from the trees' own impostors. A tree is one
// quad: the tree atlas's view nearest the cluster view's direction (turned
// into the tree's frame by its yaw), placed at the tree's bounding sphere in
// the cluster view's orthographic picture. The fragment writes the tree's
// albedo, its normal turned to the world, and the depth of the point it
// shows, so the trees of a cluster hide each other as the meshes would.
// Composed after impostor_octahedral.wgsl.
struct BakeU {
  // x = the canopy's views per side, y = the trees' views per side,
  // z = the alpha cutoff
  a: vec4<f32>,
};
@group(0) @binding(0) var<uniform> U: BakeU;
// Per tree two vec4: (foot x, y, z, yaw), (height, the tree's subject in the
// tree atlas, its cluster, 0)
@group(0) @binding(1) var<storage, read> trees: array<vec4<f32>>;
// Per cluster three vec4: (centre x, y, z, radius) in the world, its page
// (layer, origin u, v, size) and its box (half sides over the radius, 1):
// each view is framed to the box (impostorExtents)
@group(0) @binding(2) var<storage, read> clusters: array<vec4<f32>>;
// The tree atlas's frames (per subject three vec4: sphere over height, page,
// framing; pushImpostorFrame)
@group(0) @binding(3) var<storage, read> treeFrames: array<vec4<f32>>;
@group(0) @binding(4) var treeAlbedo: texture_2d_array<f32>;
@group(0) @binding(5) var treeNormalDepth: texture_2d_array<f32>;
@group(0) @binding(6) var treeSampler: sampler;

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  // Where the fragment is on the tree's picture (its layer's uv)
  @location(0) treeUv: vec2<f32>,
  @location(1) @interpolate(flat) treeLayer: i32,
  // Where it is on the cluster's picture (-1..1); a fragment outside the
  // view's cell is cut
  @location(2) local: vec2<f32>,
  // The depth of the tree's plane in the cluster's view, and the tree's
  // radius, over the cluster's radius
  @location(3) @interpolate(flat) depthRadius: vec2<f32>,
  @location(4) @interpolate(flat) yaw: f32,
};

@vertex
fn vs(@builtin(instance_index) ii: u32, @builtin(vertex_index) vi: u32) -> VsOut {
  var corner = array<vec2<f32>, 6>(
    vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),
    vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0), vec2<f32>(0.0, 1.0));
  let views = U.a.x;
  let viewCount = u32(views * views);
  let tree = ii / viewCount;
  let index = ii % viewCount;
  let cell = vec2<f32>(f32(index % u32(views)), f32(index / u32(views)));
  let foot = trees[tree * 2u];
  let extra = trees[tree * 2u + 1u];
  let yaw = foot.w;
  let height = extra.x;
  let subject = u32(extra.y);
  let cluster = u32(extra.z);
  let clusterSphere = clusters[cluster * 3u];
  let clusterPage = clusters[cluster * 3u + 1u];
  let clusterBox = clusters[cluster * 3u + 2u];
  let treeFrame = treeFrames[subject * 3u];
  let treePage = treeFrames[subject * 3u + 1u];
  // The cluster's view, and the tree's own view nearest it
  let direction = impostorViewDirection(cell, views);
  let treeViews = U.a.y;
  let treeCell = impostorNearestView(impostorUnturn(direction, yaw), treeViews);
  let treeDirection = impostorViewDirection(treeCell, treeViews);
  // The quad: the tree's picture plane, through its sphere's centre
  let treeCentre = foot.xyz + impostorTurn(treeFrame.xyz, yaw) * height;
  let treeRadius = treeFrame.w * height;
  let c = corner[vi];
  let treeLocal = c * 2.0 - vec2<f32>(1.0);
  let offset = impostorRight(treeDirection) * treeLocal.x + impostorUp(treeDirection) * treeLocal.y;
  let point = (treeCentre + impostorTurn(offset, yaw) * treeRadius - clusterSphere.xyz) / clusterSphere.w;
  let extents = impostorExtents(direction, clusterBox);
  let local = vec2<f32>(dot(point, impostorRight(direction)), dot(point, impostorUp(direction))) / extents.xy;
  let uv = impostorPageUv(clusterPage.yzw, impostorAtlasUv(cell, views, local));
  var o: VsOut;
  o.pos = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0);
  o.treeUv = impostorPageUv(treePage.yzw, impostorAtlasUv(treeCell, treeViews, treeLocal));
  o.treeLayer = i32(treePage.x);
  o.local = local;
  // Depth toward the view's eye, over the box's reach that way
  let centreDepth = dot(treeCentre - clusterSphere.xyz, direction) / (clusterSphere.w * extents.z);
  o.depthRadius = vec2<f32>(centreDepth, treeRadius / (clusterSphere.w * extents.z));
  o.yaw = yaw;
  return o;
}

struct FsOut {
  @location(0) albedo: vec4<f32>,
  @location(1) normalDepth: vec4<f32>,
  @builtin(frag_depth) depth: f32,
};

@fragment
fn fs(in: VsOut) -> FsOut {
  let albedo = textureSample(treeAlbedo, treeSampler, in.treeUv, in.treeLayer);
  let normalDepth = textureSample(treeNormalDepth, treeSampler, in.treeUv, in.treeLayer);
  if (albedo.a < U.a.z || abs(in.local.x) > 1.0 || abs(in.local.y) > 1.0) {
    discard;
  }
  // The point's depth toward the cluster view's eye: the tree's plane, moved
  // by the tree's baked depth (its own view is within a few degrees)
  let depth = clamp(in.depthRadius.x + impostorDepthOf(normalDepth, albedo.a) * in.depthRadius.y, -1.0, 1.0);
  let normal = impostorTurn(impostorUnpackNormal(normalDepth.xy), in.yaw);
  // The tree's depth behind its surface there, in the cluster's units
  let behind = impostorThicknessOf(normalDepth, albedo.a) * in.depthRadius.y;
  var o: FsOut;
  o.albedo = vec4<f32>(albedo.rgb, 1.0);
  o.normalDepth = vec4<f32>(impostorPackNormal(normal), clamp(behind * 0.5, 0.0, 1.0), 0.5 + 0.5 * depth);
  // Nearer the eye is a smaller depth (cleared to 1, test Less)
  o.depth = 0.5 - 0.5 * depth;
  return o;
}
