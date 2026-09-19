struct Frame {
  viewProj: mat4x4<f32>,
  prevViewProj: mat4x4<f32>,
  jitter: vec4<f32>,
};
struct DrawU {
  model: mat4x4<f32>,
  prevModel: mat4x4<f32>,
  albedoMetallic: vec4<f32>,
  params: vec4<f32>,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> draws: array<DrawU>;
struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) nrm: vec3<f32>,
  @location(1) @interpolate(flat) inst: u32,
  @location(2) clipNow: vec4<f32>,
  @location(3) clipPrev: vec4<f32>,
  @location(4) wpos: vec3<f32>,
  @location(5) uv: vec2<f32>,
};
struct BrickOut { albedo: vec3<f32>, nrm: vec3<f32> };
fn gbBrick(wpos: vec3<f32>, n: vec3<f32>, base: vec3<f32>) -> BrickOut {
  let an = abs(n);
  if (n.z > 0.6) { var top: BrickOut; top.albedo = base * 0.9; top.nrm = n; return top; }
  var u: f32; var v: f32; var tU: vec3<f32>; var tV: vec3<f32>;
  if (an.z >= an.x && an.z >= an.y) { u = wpos.x; v = wpos.y; tU = vec3<f32>(1.0,0.0,0.0); tV = vec3<f32>(0.0,1.0,0.0); }
  else if (an.x >= an.y) { u = wpos.y; v = wpos.z; tU = vec3<f32>(0.0,1.0,0.0); tV = vec3<f32>(0.0,0.0,1.0); }
  else { u = wpos.x; v = wpos.z; tU = vec3<f32>(1.0,0.0,0.0); tV = vec3<f32>(0.0,0.0,1.0); }
  let bw = 1.35; let bh = 0.58;
  let row = floor(v / bh);
  let stagger = (row - 2.0 * floor(row * 0.5)) * (bw * 0.5);
  let uu = u + stagger;
  let cu = fract(uu / bw);
  let cv = fract(v / bh);
  let du = min(cu, 1.0 - cu) * bw;
  let dv = min(cv, 1.0 - cv) * bh;
  let dm = min(du, dv);
  let mortarHalf = 0.05;
  let bevel = 0.14;
  let mortar = 1.0 - smoothstep(mortarHalf, mortarHalf + bevel, dm);
  let bid = floor(uu / bw) * 3.0 + row * 7.0;
  let vary = fract(sin(bid * 12.9898) * 43758.5453);
  let brickCol = base * (0.92 + 0.14 * vary);
  let mortarCol = base * vec3<f32>(0.70, 0.68, 0.66);
  var o: BrickOut;
  o.albedo = mix(brickCol, mortarCol, mortar);
  let bevelAmt = mortar * (1.0 - mortar) * 4.0;
  var perturb = vec3<f32>(0.0);
  if (du <= dv) { let dir = select(1.0, -1.0, cu < 0.5); perturb = tU * dir; }
  else { let dir = select(1.0, -1.0, cv < 0.5); perturb = tV * dir; }
  o.nrm = normalize(n + perturb * (bevelAmt * 0.55));
  return o;
}
fn gbRoofTiles(wpos: vec3<f32>, n: vec3<f32>, base: vec3<f32>) -> BrickOut {
  let hl = max(length(vec2<f32>(n.x, n.y)), 1e-4);
  let horizN = vec2<f32>(n.x, n.y) / hl;
  let tU = vec3<f32>(-horizN.y, horizN.x, 0.0);
  let tV = normalize(cross(tU, n));
  let hu = dot(wpos, tU);
  let hv = dot(wpos, tV);
  let tw = 0.72; let th = 0.55;
  let row = floor(hv / th);
  let stag = (row - 2.0 * floor(row * 0.5)) * 0.5;
  let cu = fract(hu / tw + stag) - 0.5;
  let cv = fract(hv / th) - 0.32;
  let dxs = cu;
  let dys = cv * 0.92;
  let rr = sqrt(dxs * dxs + dys * dys);
  let edge = smoothstep(0.33, 0.47, rr);
  let dome = 1.0 - edge;
  let bid = floor(hu / tw + stag) * 5.0 + row * 11.0;
  let vary = fract(sin(bid * 34.11) * 4113.7);
  var o: BrickOut;
  o.albedo = base * (0.74 + 0.30 * dome + 0.14 * vary) * (1.0 - 0.34 * edge);
  let slope = smoothstep(0.04, 0.40, rr) * dome;
  var dir = vec2<f32>(dxs, dys);
  if (rr > 1e-4) { dir = dir / rr; }
  let perturb = tU * dir.x + tV * dir.y;
  o.nrm = normalize(n + perturb * (slope * 0.95));
  return o;
}
fn gbEmblem(uv: vec2<f32>, base: vec3<f32>, codeF: f32) -> vec3<f32> {
  let vc = 0.17; let ru = 0.34; let rv = 0.085;
  let a = (uv.x - 0.5) / ru;
  let b = (uv.y - vc) / rv;
  let r = length(vec2<f32>(a, b));
  if (r > 1.18) { return base; }
  let white = vec3<f32>(0.95, 0.95, 0.92);
  if (abs(r - 0.92) < 0.13) { return white; }
  if (r >= 0.80) { return base; }
  var em = false;
  let code = i32(round(codeF));
  if (code >= 12) {
    em = (abs(r - 0.58) < 0.15) && !(b > 0.28 && abs(a) < 0.44);
  } else if (code == 11) {
    em = (abs(a - b) < 0.16 || abs(a + b) < 0.16) && r < 0.70;
  } else {
    let bd = length(vec2<f32>(a + 0.70, b * 0.9));
    let bow = (abs(bd - 0.55) < 0.12) && a < 0.05;
    let shaft = (abs(b) < 0.11) && a > -0.55 && a < 0.55;
    let hu = (abs(b - (0.55 - a)) < 0.13) && a > 0.12 && a < 0.55;
    let hd = (abs(b + (0.55 - a)) < 0.13) && a > 0.12 && a < 0.55;
    em = bow || shaft || hu || hd;
  }
  if (em) { return white; }
  return base;
}
@vertex
fn vs(@builtin(instance_index) ii: u32,
      @location(0) p: vec3<f32>, @location(1) n: vec3<f32>, @location(2) uv: vec2<f32>) -> VsOut {
  let d = draws[ii];
  var o: VsOut;
  let world = d.model * vec4<f32>(p, 1.0);
  o.wpos = world.xyz;
  o.pos = F.viewProj * world;
  o.nrm = normalize((d.model * vec4<f32>(n, 0.0)).xyz);
  o.uv = uv;
  o.inst = ii;
  o.clipNow = o.pos;
  o.clipPrev = F.prevViewProj * (d.prevModel * vec4<f32>(p, 1.0));
  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);
  return o;
}
struct FsOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
};
@fragment
fn fs(in: VsOut) -> FsOut {
  let d = draws[in.inst];
  let n = normalize(in.nrm);
  let oct = octEncode(n);
  let rough = clamp(d.params.x, 0.045, 1.0);
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  var albedoOut = d.albedoMetallic.rgb;
  var nrmOut = n;
  if (d.params.w > 9.5) {
    albedoOut = gbEmblem(in.uv, albedoOut, d.params.w);
  } else if (d.params.w > 1.5) {
    let rt = gbRoofTiles(in.wpos, n, albedoOut);
    albedoOut = rt.albedo;
    nrmOut = rt.nrm;
  } else if (d.params.w > 0.5) {
    let bk = gbBrick(in.wpos, n, albedoOut);
    albedoOut = bk.albedo;
    nrmOut = bk.nrm;
  }
  let octB = octEncode(nrmOut);
  var o: FsOut;
  o.gba = vec4<f32>(octB.x, octB.y, 0.5, d.params.z);
  o.gbb = vec4<f32>(albedoOut, rough);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y,
                     clamp(d.albedoMetallic.a, 0.0, 1.0), d.params.y);
  return o;
}
