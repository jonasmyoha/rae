// The view grid of an octahedral impostor (docs/tree-lod-design.md §2.2),
// shared by the bake (impostor_bake.wgsl) and the draws that read the atlas.
//
// A prop is baked from `views` x `views` directions over the upper
// hemisphere (hemi-octahedral: a prop is seen from the side and from above,
// never from under the ground). Grid point (i, j) is the direction at
// uv = (i, j) / (views - 1): the square's centre looks straight down, and its
// whole border is the horizon, so the outer ring of views are side views.
// A direction here points from the prop TOWARD the eye, z up.
//
// The atlas has one cell per view: cell (i, j) is column i, row j, each an
// orthographic picture of the prop's bounding sphere (centre, radius): the
// view's right along +u, its up along -v (row 0 is the top), the sphere's
// rim touching the cell's sides.

// The direction of grid uv (0..1 each)
fn impostorDirection(uv: vec2<f32>) -> vec3<f32> {
  let p = uv * 2.0 - vec2<f32>(1.0);
  let x = (p.x + p.y) * 0.5;
  let y = (p.x - p.y) * 0.5;
  let z = max(1.0 - abs(x) - abs(y), 0.0);
  return normalize(vec3<f32>(x, y, z));
}

// The grid uv (0..1 each) of a direction; one below the horizon is taken
// at the horizon
fn impostorGridUv(direction: vec3<f32>) -> vec2<f32> {
  let d = vec3<f32>(direction.xy, max(direction.z, 0.0));
  let sum = abs(d.x) + abs(d.y) + d.z;
  if (sum < 0.000001) {
    return vec2<f32>(0.5);
  }
  let o = d / sum;
  return vec2<f32>(o.x + o.y, o.x - o.y) * 0.5 + vec2<f32>(0.5);
}

// The grid point (i, j) nearest to a direction
fn impostorNearestView(direction: vec3<f32>, views: f32) -> vec2<f32> {
  return round(impostorGridUv(direction) * (views - 1.0));
}

// The direction of grid point (i, j)
fn impostorViewDirection(cell: vec2<f32>, views: f32) -> vec3<f32> {
  return impostorDirection(cell / (views - 1.0));
}

// A view's right axis: horizontal, to the right of an eye looking back along
// `direction` with z up (+y for the view from +x); straight down, +y
fn impostorRight(direction: vec3<f32>) -> vec3<f32> {
  let r = vec3<f32>(-direction.y, direction.x, 0.0);
  let l = length(r);
  if (l < 0.0001) {
    return vec3<f32>(0.0, 1.0, 0.0);
  }
  return r / l;
}

// A view's up axis (z for a side view)
fn impostorUp(direction: vec3<f32>) -> vec3<f32> {
  return cross(direction, impostorRight(direction));
}

// The uv in a layer of a point of its page `page` (origin u, origin v, size:
// many pages share a layer when there are many subjects, the far canopy's)
fn impostorPageUv(page: vec3<f32>, uv: vec2<f32>) -> vec2<f32> {
  return page.xy + uv * page.z;
}

// The atlas uv of a point of view (i, j)'s picture: `local` is -1..1 across
// the sphere along the view's right (x) and up (y)
fn impostorAtlasUv(cell: vec2<f32>, views: f32, local: vec2<f32>) -> vec2<f32> {
  return vec2<f32>(cell.x + 0.5 + 0.5 * local.x, cell.y + 0.5 - 0.5 * local.y) / views;
}

// The atlas's normal-and-depth texel (both bakes write it, the mips average
// it, the draws read it): the normal octahedral in .xy, how deep the prop is
// behind its front surface there in .z (0..1 = 0..2 depth units: about a
// crown, almost nothing for a trunk; impostorMarch tests against it) and the
// front surface's depth in .w (0.5 + 0.5 * depth, 1 at the sphere's or
// box's front). An empty texel is (0.5, 0.5, 0, 0). The .z and .w are
// stored times the texel's coverage (the albedo's alpha): a filtered read
// at a prop's edge mixes in empty texels, and dividing by the filtered
// alpha (impostorDepthOf, impostorThicknessOf) takes them out again, where
// a plain read would see the edge far behind the prop.
fn impostorDepthOf(normalDepth: vec4<f32>, alpha: f32) -> f32 {
  return clamp(normalDepth.w / max(alpha, 0.001), 0.0, 1.0) * 2.0 - 1.0;
}
fn impostorThicknessOf(normalDepth: vec4<f32>, alpha: f32) -> f32 {
  return clamp(normalDepth.z / max(alpha, 0.001), 0.0, 1.0) * 2.0;
}
fn impostorPackNormal(normal: vec3<f32>) -> vec2<f32> {
  let n = normal / max(abs(normal.x) + abs(normal.y) + abs(normal.z), 0.000001);
  var p = n.xy;
  if (n.z < 0.0) {
    p = (vec2<f32>(1.0) - abs(n.yx)) * vec2<f32>(select(-1.0, 1.0, n.x >= 0.0), select(-1.0, 1.0, n.y >= 0.0));
  }
  return p * 0.5 + vec2<f32>(0.5);
}
fn impostorUnpackNormal(packed: vec2<f32>) -> vec3<f32> {
  let f = packed * 2.0 - vec2<f32>(1.0);
  var n = vec3<f32>(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
  let t = max(-n.z, 0.0);
  n = vec3<f32>(n.x + select(t, -t, n.x >= 0.0), n.y + select(t, -t, n.y >= 0.0), n.z);
  return normalize(n);
}

// The three grid points round a direction and their weights (the
// barycentric weights of the direction in the grid triangle it falls in;
// they sum to 1). Blending the three views by them changes the picture
// smoothly as the eye moves round the prop.
struct ImpostorViews {
  a: vec2<f32>,
  b: vec2<f32>,
  c: vec2<f32>,
  weights: vec3<f32>,
};
fn impostorThreeViews(direction: vec3<f32>, views: f32) -> ImpostorViews {
  let last = views - 1.0;
  let grid = clamp(impostorGridUv(direction) * last, vec2<f32>(0.0), vec2<f32>(last));
  let base = min(floor(grid), vec2<f32>(last - 1.0));
  let f = grid - base;
  var result: ImpostorViews;
  result.b = base + vec2<f32>(1.0, 0.0);
  result.c = base + vec2<f32>(0.0, 1.0);
  if (f.x + f.y < 1.0) {
    result.a = base;
    result.weights = vec3<f32>(1.0 - f.x - f.y, f.x, f.y);
  } else {
    result.a = base + vec2<f32>(1.0);
    result.weights = vec3<f32>(f.x + f.y - 1.0, 1.0 - f.y, 1.0 - f.x);
  }
  return result;
}

// How far a subject reaches along a view's right, up and toward its eye,
// over its radius. A sphere framed subject (a tree) reaches 1 every way; a
// box framed one (a canopy cluster: `box.w` = 1, `box.xyz` the box's half
// sides over the radius) its box's outline on the view, so each view's
// picture holds the box and no more (a wide, low cluster fills its side
// views top to bottom).
fn impostorExtents(viewDirection: vec3<f32>, box: vec4<f32>) -> vec3<f32> {
  if (box.w < 0.5) {
    return vec3<f32>(1.0);
  }
  let reach = vec3<f32>(
    dot(abs(impostorRight(viewDirection)), box.xyz),
    dot(abs(impostorUp(viewDirection)), box.xyz),
    dot(abs(viewDirection), box.xyz));
  return max(reach, vec3<f32>(0.001));
}

// Where on view `viewDirection`'s picture (-1..1 along its right and up) the
// ray toward the eye's direction `eye` through `point` crosses the view's
// plane (both in the prop's own frame, `point` relative to the sphere's
// centre, over its radius), the view framed to `extents`
fn impostorViewPoint(point: vec3<f32>, eye: vec3<f32>, viewDirection: vec3<f32>, extents: vec3<f32>) -> vec2<f32> {
  let along = dot(point, viewDirection) / max(dot(eye, viewDirection), 0.2);
  let onPlane = point - eye * along;
  return vec2<f32>(dot(onPlane, impostorRight(viewDirection)), dot(onPlane, impostorUp(viewDirection))) / extents.xy;
}

// The parallax of a baked depth: the picture point a ray toward `eye`
// reaches at the prop's surface, `depth` (-1..1, toward the view's eye)
// above the view's plane, from where it crosses the plane
fn impostorParallax(
  local: vec2<f32>, depth: f32, eye: vec3<f32>, viewDirection: vec3<f32>, extents: vec3<f32>,
) -> vec2<f32> {
  let facing = max(dot(eye, viewDirection), 0.2);
  let across = vec2<f32>(dot(eye, impostorRight(viewDirection)), dot(eye, impostorUp(viewDirection)));
  let shift = across / extents.xy * (depth * extents.z / facing);
  return clamp(local + shift, vec2<f32>(-1.0), vec2<f32>(1.0));
}

// The point of view `viewDirection`'s picture at `local`, `depth` above its
// plane, in the prop's own frame (relative to the centre, over the radius)
fn impostorViewPosition(local: vec2<f32>, depth: f32, viewDirection: vec3<f32>, extents: vec3<f32>) -> vec3<f32> {
  return impostorRight(viewDirection) * (local.x * extents.x) + impostorUp(viewDirection) * (local.y * extents.y)
    + viewDirection * (depth * extents.z);
}

// A vector turned by `yaw` about z (the prop's frame to the world), and back
fn impostorTurn(v: vec3<f32>, yaw: f32) -> vec3<f32> {
  let c = cos(yaw);
  let s = sin(yaw);
  return vec3<f32>(v.x * c - v.y * s, v.x * s + v.y * c, v.z);
}
fn impostorUnturn(v: vec3<f32>, yaw: f32) -> vec3<f32> {
  return impostorTurn(v, -yaw);
}
