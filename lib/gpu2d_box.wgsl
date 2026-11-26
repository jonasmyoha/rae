// gpu2d_box.wgsl — the 2D box uber-shader (#110), a Rae asset since #907
// (was the C constant G2D_BOX_WGSL). Instanced rounded-box SDF with analytic
// AA: one quad per primitive; the fragment shader evaluates a rounded-box
// signed distance and antialiases with screen-space derivatives (no MSAA).
// One pipeline -> filled/rounded rects, per-corner radius, borders, gradients,
// rotated capsules (lines), ring arcs. Instance layout = 6 x vec4 (std430):
// rect, radius, fill, border, params, grad. params = (border width, angle,
// mode, gradient angle): mode 1 a gradient, mode 2 a ring arc (Gpu2dCanvasArc:
// the ring inside the square rect, params.x its thickness, params.w the share
// of the turn drawn clockwise from the top, round ends), mode 3 a radial
// gradient (Gpu2dCanvasRadial: `border` holds its center as a fraction of the
// box, its radius as a fraction of the distance from the center to the
// farthest corner, and the share of that radius where the color reaches `grad`;
// fill at the center, grad from the stop out), mode 4 a cubic Bezier stroke
// (Gpu2dCanvasCurve: its four points relative to the rect's corner in `radius`
// (p0, p1) and `border` (p2, p3), params.x its width, grad.x how many straight
// pieces the distance is measured against).
struct Prim {
  rect: vec4<f32>,
  radius: vec4<f32>,
  fill: vec4<f32>,
  border: vec4<f32>,
  params: vec4<f32>,
  grad: vec4<f32>,
};
// uXform[0] = (physW, physH, scaleX, scaleY); uXform[1] = (offsetX, offsetY,..)
// maps design-unit coords -> physical px: px = design*scale + offset.
@group(0) @binding(0) var<uniform> uXform: array<vec4<f32>, 2>;
@group(0) @binding(1) var<storage, read> prims: array<Prim>;
// #118 rounded clip, in its node's frame: uClip[0]=(origin design units, size in the
// node's units), uClip[1]=(radius in the node's units, enabled,..), uClip[2]=the 2x2
// from a design offset into the node's frame (Gpu2dCanvasClip clipUniformFor)
@group(0) @binding(2) var<uniform> uClip: array<vec4<f32>, 3>;
// The clip in its node's own frame (Gpu2dCanvasClip): the design offset from its origin
// taken into the frame by the 2x2, then the rounded box there, so a turned or scaled
// node's corners stay round and the smoothing (fwidth in that frame) is about a pixel
fn clipShapeDistance(posD: vec2<f32>, origin: vec2<f32>, size: vec2<f32>, frame: vec4<f32>, radius: f32) -> f32 {
  let offset = posD - origin;
  let local = vec2<f32>(frame.x * offset.x + frame.y * offset.y, frame.z * offset.x + frame.w * offset.y);
  return sdRoundBoxClip(local - size * 0.5, size * 0.5, radius);
}
fn sdRoundBoxClip(p: vec2<f32>, b: vec2<f32>, r: f32) -> f32 {
  let rr = min(r, min(b.x, b.y));
  let q = abs(p) - b + vec2<f32>(rr, rr);
  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - rr;
}
struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) local: vec2<f32>,
  @location(1) @interpolate(flat) inst: u32,
  @location(2) posD: vec2<f32>,
};
@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {
  var corners = array<vec2<f32>, 6>(
    vec2<f32>(0.0,0.0), vec2<f32>(1.0,0.0), vec2<f32>(0.0,1.0),
    vec2<f32>(0.0,1.0), vec2<f32>(1.0,0.0), vec2<f32>(1.0,1.0));
  let c = corners[vi];
  let p = prims[ii];
  let phys = uXform[0].xy;
  let local = c * p.rect.zw;  // box-local, unrotated (0..w, 0..h)
  let center = p.rect.zw * 0.5;
  let a = p.params.y;  // rotation (radians), 0 for rects
  let ca = cos(a); let sa = sin(a);
  let rel = local - center;
  let rot = vec2<f32>(rel.x * ca - rel.y * sa, rel.x * sa + rel.y * ca);
  let posDesign = p.rect.xy + center + rot;
  let posPx = posDesign * uXform[0].zw + uXform[1].xy;
  let ndc = vec2<f32>(posPx.x / phys.x * 2.0 - 1.0,
                      1.0 - posPx.y / phys.y * 2.0);
  var o: VsOut;
  o.pos = vec4<f32>(ndc, 0.0, 1.0);
  o.local = local;  // SDF evaluates in unrotated box frame
  o.inst = ii;
  o.posD = posDesign;  // design-space pos for the clip SDF
  return o;
}
// Distance from `p` to the segment a..b
fn sdSegment(p: vec2<f32>, a: vec2<f32>, b: vec2<f32>) -> f32 {
  let pa = p - a;
  let ba = b - a;
  let h = clamp(dot(pa, ba) / max(dot(ba, ba), 0.000001), 0.0, 1.0);
  return length(pa - ba * h);
}
// Distance from `p` to a cubic Bezier p0..p3, measured against `pieces`
// straight pieces of it
fn sdCubic(p: vec2<f32>, p0: vec2<f32>, p1: vec2<f32>, p2: vec2<f32>, p3: vec2<f32>, pieces: i32) -> f32 {
  var prev = p0;
  var best = 1.0e9;
  for (var i = 1; i <= pieces; i = i + 1) {
    let t = f32(i) / f32(pieces);
    let u = 1.0 - t;
    let q = u * u * u * p0 + 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t * p3;
    best = min(best, sdSegment(p, prev, q));
    prev = q;
  }
  return best;
}
fn sdRoundBox(p: vec2<f32>, b: vec2<f32>, r: vec4<f32>) -> f32 {
  let rad = select(r.zw, r.xy, p.x > 0.0);
  let rr = select(rad.y, rad.x, p.y > 0.0);
  let q = abs(p) - b + vec2<f32>(rr, rr);
  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - rr;
}
// A ring of outer radius `outer` and `thickness`, drawn from the top
// clockwise (screen y down) over `share` of the turn, with round ends
fn sdRingArc(p: vec2<f32>, outer: f32, thickness: f32, share: f32) -> f32 {
  let half = thickness * 0.5;
  let mid = outer - half;
  let ring = abs(length(p) - mid) - half;
  if (share >= 0.9999) {
    return ring;
  }
  var turn = atan2(p.x, -p.y) / 6.2831853;
  if (turn < 0.0) {
    turn = turn + 1.0;
  }
  if (turn <= share) {
    return ring;
  }
  let endAngle = clamp(share, 0.0, 1.0) * 6.2831853;
  let start = vec2<f32>(0.0, -mid);
  let end = vec2<f32>(sin(endAngle) * mid, -cos(endAngle) * mid);
  return min(length(p - start), length(p - end)) - half;
}
@fragment
fn fs(in: VsOut) -> @location(0) vec4<f32> {
  let p = prims[in.inst];
  let halfSize = p.rect.zw * 0.5;
  let center = in.local - halfSize;
// CLAMP THE RADIUS. sdRoundBox is only defined for r <= min(halfSize);
// beyond that `abs(p) - b + r` is positive everywhere and the rounded box
// degenerates into a pointed shape. A "pill" radius is authored as a number
// large enough to round any button (20 in the app3d theme), so every control
// shorter than 40 units hit this — the camera bar's 34-tall buttons rendered
// as spear tips rather than pills. Clamping here rather than at each call
// site also covers buttons whose height only becomes small after layout.
  let rmax = min(halfSize.x, halfSize.y);
  let rad4 = min(p.radius, vec4<f32>(rmax, rmax, rmax, rmax));
  var d = sdRoundBox(center, halfSize, rad4);
  let arc = p.params.z > 1.5 && p.params.z < 2.5;
  let radial = p.params.z > 2.5 && p.params.z < 3.5;
  let curve = p.params.z > 3.5;
  if (arc) {
    d = sdRingArc(center, rmax, p.params.x, p.params.w);
  }
  if (curve) {
    let pieces = i32(clamp(p.grad.x, 2.0, 64.0));
    d = sdCubic(in.local, p.radius.xy, p.radius.zw, p.border.xy, p.border.zw, pieces) - p.params.x * 0.5;
  }
  let aa = max(fwidth(d), 0.0001);
  var cov = 1.0 - smoothstep(-aa, aa, d);
  if (uClip[1].y > 0.5) {  // #118: rounded clip coverage
    let cd = clipShapeDistance(in.posD, uClip[0].xy, uClip[0].zw, uClip[2], uClip[1].x);
    let caa = max(fwidth(cd), 0.0001);
    cov = cov * (1.0 - smoothstep(-caa, caa, cd));
  }
  var col = p.fill;
  if (arc || curve) {
    return col * cov;
  }
  if (radial) {
    // Per pixel, so a soft glow has no rings. A dither of half a step of
    // 8-bit color breaks the bands a slow dark gradient would show.
    let size = max(p.rect.zw, vec2<f32>(1.0, 1.0));
    let focus = p.border.xy * size;
    let far = max(max(length(focus), length(focus - vec2<f32>(size.x, 0.0))),
                  max(length(focus - vec2<f32>(0.0, size.y)), length(focus - size)));
    let reach = max(p.border.z * far * max(p.border.w, 0.0001), 0.0001);
    let t = clamp(length(in.local - focus) / reach, 0.0, 1.0);
    col = mix(p.fill, p.grad, t);
    let noise = fract(52.9829189 * fract(dot(in.pos.xy, vec2<f32>(0.06711056, 0.00583715)))) - 0.5;
    let rgb = clamp(col.rgb + vec3<f32>(noise / 255.0), vec3<f32>(0.0), vec3<f32>(col.a));
    return vec4<f32>(rgb, col.a) * cov;
  }
  if (p.params.z > 0.5) {
    let uv = in.local / max(p.rect.zw, vec2<f32>(1.0, 1.0));
    let dir = vec2<f32>(cos(p.params.w), sin(p.params.w));
    let centered = uv - vec2<f32>(0.5, 0.5);
    let extent = max(abs(dir.x) * 0.5 + abs(dir.y) * 0.5, 0.0001);
    let t = clamp(dot(centered, dir) / (extent * 2.0) + 0.5, 0.0, 1.0);
    col = mix(p.fill, p.grad, t);
  }
  let bw = p.params.x;
  if (bw > 0.0) {
    let inner = 1.0 - smoothstep(-aa, aa, d + bw);
    col = mix(p.border, p.fill, inner);
  }
  return col * cov;
}
