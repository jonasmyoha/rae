// gpu2d_box.wgsl — the 2D box uber-shader (#110), a Rae asset since #907
// (was the C constant G2D_BOX_WGSL). Instanced rounded-box SDF with analytic
// AA: one quad per primitive; the fragment shader evaluates a rounded-box
// signed distance and antialiases with screen-space derivatives (no MSAA).
// One pipeline -> filled/rounded rects, per-corner radius, borders, gradients,
// rotated capsules (lines). Instance layout = 6 x vec4 (std430): rect, radius,
// fill, border, params, grad.
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
// #118 rounded clip: uClip[0]=(x,y,w,h) design units, uClip[1]=(radius,enabled,..)
@group(0) @binding(2) var<uniform> uClip: array<vec4<f32>, 2>;
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
fn sdRoundBox(p: vec2<f32>, b: vec2<f32>, r: vec4<f32>) -> f32 {
  let rad = select(r.zw, r.xy, p.x > 0.0);
  let rr = select(rad.y, rad.x, p.y > 0.0);
  let q = abs(p) - b + vec2<f32>(rr, rr);
  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - rr;
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
  let d = sdRoundBox(center, halfSize, rad4);
  let aa = max(fwidth(d), 0.0001);
  var cov = 1.0 - smoothstep(-aa, aa, d);
  if (uClip[1].y > 0.5) {  // #118: rounded clip coverage
    let cc = uClip[0].xy + uClip[0].zw * 0.5;
    let ch = uClip[0].zw * 0.5;
    let cr = uClip[1].x;
    let cd = sdRoundBox(in.posD - cc, ch, vec4<f32>(cr, cr, cr, cr));
    let caa = max(fwidth(cd), 0.0001);
    cov = cov * (1.0 - smoothstep(-caa, caa, cd));
  }
  var col = p.fill;
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
