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

// The atlas uv of a point of view (i, j)'s picture: `local` is -1..1 across
// the sphere along the view's right (x) and up (y)
fn impostorAtlasUv(cell: vec2<f32>, views: f32, local: vec2<f32>) -> vec2<f32> {
  return vec2<f32>(cell.x + 0.5 + 0.5 * local.x, cell.y + 0.5 - 0.5 * local.y) / views;
}

// The light the far level was lit with before the impostors (the CPU
// picture's, app3d/SpriteBake spriteBakeLight): from the view's upper left,
// toward the eye. The impostor draw lights the baked albedo with it and the
// baked normal, so the far trees keep their look until they are lit through
// the G-buffer like the meshes (tree LOD step 5).
fn impostorBakeLight(direction: vec3<f32>) -> vec3<f32> {
  return normalize(
    impostorRight(direction) * -0.45 + direction * 0.55 + impostorUp(direction) * 0.7);
}

// The shade of that light on a baked normal (-1..1, object space)
fn impostorBakeShade(normal: vec3<f32>, direction: vec3<f32>) -> f32 {
  let n = normalize(normal + vec3<f32>(0.0, 0.0, 0.00001));
  return 0.62 + 0.48 * max(dot(n, impostorBakeLight(direction)), 0.0);
}
