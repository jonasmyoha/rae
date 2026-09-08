// waterFft.wgsl — the FFT ocean's one-time bakes (#849): the initial spectrum
// h0(k) per cascade and the Stockham butterfly table. Technique after
// Tessendorf / Hasselmann et al.; written from the papers, no code copied.
//
// spectrum: for every texel of an N x N grid covering one cascade tile of L
// metres, the wave vector is k = 2pi/L * (x - N/2, y - N/2). The energy at k is
// the JONSWAP frequency spectrum S(w) (Hasselmann 1973, fetch-limited, with the
// TMA finite-depth factor of Bouws 1985) times the Hasselmann 1980 directional
// spreading D(theta), times the Jacobian (dw/dk)/k that turns an (w, theta)
// density into a k-plane density. h0 is a complex Gaussian draw scaled by
// sqrt(2 E(k)) dk. The texel packs (h0(k), h0(-k)) so the evolution shader can
// build a Hermitian h(k, t) and keep the surface real; each cascade owns a k
// band [kLow, kHigh) so the three tiles never count a wavelength twice.
//
// butterfly: the per-output radix-2 Stockham table for an N-point INVERSE FFT,
// one column per stage: at stage s (span Ns = 2^s), output i belongs to block
// i / 2Ns at offset k = i mod 2Ns and is a +/- combination of inputs
// j = block*Ns + (k mod Ns) and j + N/2 with twiddle exp(+2pi i (k mod Ns)/2Ns)
// (the sign folded into the twiddle for the second half). Autosort: no bit
// reversal, the same table serves rows and columns.
struct FftU {
  grid: vec4<f32>,   // x = N, y = tile L (m), z = wind speed U (m/s), w = wind direction (rad)
  sea: vec4<f32>,    // x = fetch F (m), y = gravity g, z = depth h (m), w = seed
  band: vec4<f32>,   // x = kLow, y = kHigh, z = cascade index, w = test mode (1: one known wave)
};
@group(0) @binding(0) var<uniform> U: FftU;
@group(0) @binding(1) var h0Out: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var butterflyOut: texture_storage_2d<rgba32float, write>;

const PI: f32 = 3.14159265358979;

// PCG-style integer hash -> [0, 1). Deterministic per (seed, texel, salt), so
// the same k always draws the same Gaussian — the (k, -k) packing depends on it.
fn hashU(v: u32) -> u32 {
  var x = v * 747796405u + 2891336453u;
  x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
  return (x >> 22u) ^ x;
}
fn rand01(seed: u32, x: u32, y: u32, salt: u32) -> f32 {
  let h = hashU(hashU(hashU(seed ^ (x * 1973u)) ^ (y * 9277u)) ^ (salt * 26699u));
  return (f32(h & 0x00ffffffu) + 0.5) / 16777216.0;
}
// Box-Muller: two uniforms -> one standard complex Gaussian.
fn gaussian2(u1: f32, u2: f32) -> vec2<f32> {
  let r = sqrt(-2.0 * log(max(u1, 1e-7)));
  return vec2<f32>(r * cos(2.0 * PI * u2), r * sin(2.0 * PI * u2));
}

// ln Gamma(x) for x > 0: Stirling with the first corrections, shifted up by 3
// for small arguments so the series is accurate.
fn lnGamma(xIn: f32) -> f32 {
  var x = xIn;
  var shift = 0.0;
  if (x < 3.0) { shift = -log(x * (x + 1.0) * (x + 2.0)); x = x + 3.0; }
  return (x - 0.5) * log(x) - x + 0.5 * log(2.0 * PI) + 1.0 / (12.0 * x) - 1.0 / (360.0 * x * x * x) + shift;
}

// JONSWAP S(w) with the TMA depth factor.
fn jonswapTma(w: f32, wp: f32, alpha: f32, g: f32, depth: f32) -> f32 {
  var sigma = 0.09;
  if (w <= wp) { sigma = 0.07; }
  let r = exp(-(w - wp) * (w - wp) / (2.0 * sigma * sigma * wp * wp));
  let s = alpha * g * g / pow(w, 5.0) * exp(-1.25 * pow(wp / w, 4.0)) * pow(3.3, r);
  // TMA: waves feel the bottom.
  let wh = w * sqrt(depth / g);
  var phi = 1.0;
  if (wh < 1.0) { phi = 0.5 * wh * wh; } else if (wh < 2.0) { phi = 1.0 - 0.5 * (2.0 - wh) * (2.0 - wh); }
  return s * phi;
}

// Hasselmann 1980 directional spreading: D = N(s) cos^(2s)(theta/2), s from w/wp.
fn hasselmann(w: f32, wp: f32, theta: f32, windSpeed: f32, g: f32) -> f32 {
  var s = 0.0;
  if (w <= wp) {
    s = 6.97 * pow(max(w / wp, 1e-4), 4.06);
  } else {
    s = 9.77 * pow(w / wp, -2.33 - 1.45 * (windSpeed * wp / g - 1.17));
  }
  s = clamp(s, 0.05, 40.0);
  // Normalisation so D integrates to 1 over theta: 2^(2s-1)/pi * G(s+1)^2 / G(2s+1).
  let norm = exp((2.0 * s - 1.0) * log(2.0) + 2.0 * lnGamma(s + 1.0) - lnGamma(2.0 * s + 1.0)) / PI;
  let c = max(cos(0.5 * theta), 0.0);
  return norm * pow(c, 2.0 * s);
}

// h0 at a texel: (re, im). Zero at k = 0, on the Nyquist row/column (its -k
// is itself, which would break the packing symmetry), and outside the band.
fn h0At(px: u32, py: u32) -> vec2<f32> {
  let n = f32(U.grid.x);
  let half = u32(U.grid.x) / 2u;
  if (px == 0u || py == 0u) { return vec2<f32>(0.0, 0.0); }
  // Test mode (#850): ONE known wave, h0 = 0.25 at k index +-(3, 0), so that
  // after evolution at t = 0 (h = h0 + conj(h0(-k)) = 0.5 at both) the
  // unnormalised inverse transform is exactly cos(2 pi 3 x / N) of amplitude 1.
  if (U.band.w > 0.5) {
    if (py == half && (px == half + 3u || px + 3u == half)) { return vec2<f32>(0.25, 0.0); }
    return vec2<f32>(0.0, 0.0);
  }
  let kx = 2.0 * PI / U.grid.y * (f32(px) - f32(half));
  let ky = 2.0 * PI / U.grid.y * (f32(py) - f32(half));
  let kLen = length(vec2<f32>(kx, ky));
  if (kLen < 1e-6 || kLen < U.band.x || kLen >= U.band.y) { return vec2<f32>(0.0, 0.0); }
  let g = U.sea.y;
  let depth = U.sea.z;
  let windSpeed = U.grid.z;
  let fetch = U.sea.x;
  // tanh saturates by ~20 in f32; larger arguments overflow the GPU's
  // exp-based tanh into NaN (k h reaches 160 on the 8 m tile at 50 m depth).
  let kh = min(kLen * depth, 20.0);
  let th = tanh(kh);
  let w = sqrt(g * kLen * th);
  // dw/dk = g (tanh(kh) + kh sech^2(kh)) / (2 w)
  let sech2 = 1.0 - th * th;
  let dwdk = g * (th + kh * sech2) / (2.0 * w);
  let wp = 22.0 * pow(g * g / (windSpeed * fetch), 1.0 / 3.0);
  let alpha = 0.076 * pow(windSpeed * windSpeed / (fetch * g), 0.22);
  var theta = atan2(ky, kx) - U.grid.w;
  theta = theta - 2.0 * PI * floor((theta + PI) / (2.0 * PI));   // wrap to [-pi, pi)
  let energy = jonswapTma(w, wp, alpha, g, depth) * hasselmann(w, wp, theta, windSpeed, g) * dwdk / kLen;
  let dk = 2.0 * PI / U.grid.y;
  let amplitude = sqrt(2.0 * max(energy, 0.0)) * dk;
  let seed = u32(U.sea.w);
  let xi = gaussian2(rand01(seed, px, py, 1u), rand01(seed, px, py, 2u));
  return xi * (amplitude / sqrt(2.0));
}

@compute @workgroup_size(8, 8)
fn spectrum(@builtin(global_invocation_id) gid: vec3<u32>) {
  let n = u32(U.grid.x);
  if (gid.x >= n || gid.y >= n) { return; }
  let here = h0At(gid.x, gid.y);
  // -k: the mirrored texel; (n - 0) mod n = 0, and row/column 0 is zeroed anyway.
  let mirror = h0At((n - gid.x) % n, (n - gid.y) % n);
  textureStore(h0Out, vec2<i32>(gid.xy), vec4<f32>(here, mirror));
}

@compute @workgroup_size(8, 8)
fn butterfly(@builtin(global_invocation_id) gid: vec3<u32>) {
  let n = u32(U.grid.x);
  let stage = gid.x;
  let i = gid.y;
  var stages = 0u;
  var v = n;
  loop { if (v <= 1u) { break; } v = v >> 1u; stages = stages + 1u; }
  if (stage >= stages || i >= n) { return; }
  let ns = 1u << stage;
  let block = i / (2u * ns);
  let k = i % (2u * ns);
  let kk = k % ns;
  let j = block * ns + kk;
  let angle = 2.0 * PI * f32(kk) / f32(2u * ns);   // inverse transform: +
  var sign = 1.0;
  if (k >= ns) { sign = -1.0; }
  textureStore(butterflyOut, vec2<i32>(i32(stage), i32(i)),
    vec4<f32>(sign * cos(angle), sign * sin(angle), f32(j), f32(j + n / 2u)));
}

// ---------------------------------------------------------------------------
// Per-frame evolution + inverse FFT (#850). All bindings that are READ are
// plain texture_2d<f32> (textureLoad, core WebGPU); only destinations are
// storage textures. One compute pass per frame issues every dispatch below;
// dispatches in a pass see each other's storage writes in order.
struct EvolveU {
  grid: vec4<f32>,   // x = N, y = tile L, z = gravity, w = depth
  wave: vec4<f32>,   // x = time, y = field index, z = choppiness, w = unused
};
@group(0) @binding(0) var<uniform> E: EvolveU;
@group(0) @binding(1) var h0In: texture_2d<f32>;
@group(0) @binding(2) var evolveOut: texture_storage_2d<rgba32float, write>;

fn cmul(a: vec2<f32>, b: vec2<f32>) -> vec2<f32> {
  return vec2<f32>(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}
// i * z
fn ctimesI(z: vec2<f32>) -> vec2<f32> { return vec2<f32>(-z.y, z.x); }

// h(k, t) = h0(k) e^{i w t} + conj(h0(-k)) e^{-i w t}, then the field pair this
// dispatch packs: field f = A + i B where A and B are real-valued fields
// (their spectra are Hermitian), so the inverse FFT's real part is A and its
// imaginary part is B — two fields for one transform.
@compute @workgroup_size(8, 8)
fn evolve(@builtin(global_invocation_id) gid: vec3<u32>) {
  let n = u32(E.grid.x);
  if (gid.x >= n || gid.y >= n) { return; }
  let half = n / 2u;
  let packed = textureLoad(h0In, vec2<i32>(gid.xy), 0);
  let h0 = packed.xy;
  let h0m = packed.zw;
  let kx = 2.0 * PI / E.grid.y * (f32(gid.x) - f32(half));
  let ky = 2.0 * PI / E.grid.y * (f32(gid.y) - f32(half));
  let kLen = max(length(vec2<f32>(kx, ky)), 1e-6);
  let g = E.grid.z;
  let w = sqrt(g * kLen * tanh(min(kLen * E.grid.w, 20.0)));   // see h0At: tanh overflows past ~44
  let t = E.wave.x;
  let e = vec2<f32>(cos(w * t), sin(w * t));
  let h = cmul(h0, e) + cmul(vec2<f32>(h0m.x, -h0m.y), vec2<f32>(e.x, -e.y));
  let dx = -kx / kLen;   // Dx = -i (kx/k) h  -> (kx/k) * (h.y, -h.x)
  let dy = -ky / kLen;
  let Dx = ctimesI(h) * dx;
  let Dy = ctimesI(h) * dy;
  let dhdx = ctimesI(h) * kx;
  let dhdy = ctimesI(h) * ky;
  let dDxdx = ctimesI(Dx) * kx;
  let dDydy = ctimesI(Dy) * ky;
  let dDxdy = ctimesI(Dx) * ky;
  var a = h;
  var b = Dx;
  let field = u32(E.wave.y + 0.5);
  if (field == 1u) { a = Dy; b = dhdx; }
  if (field == 2u) { a = dhdy; b = dDxdx; }
  if (field == 3u) { a = dDydy; b = dDxdy; }
  // Z = A + i B
  let z = vec2<f32>(a.x - b.y, a.y + b.x);
  textureStore(evolveOut, vec2<i32>(gid.xy), vec4<f32>(z, 0.0, 0.0));
}

// One Stockham stage along x for every row: output i of row y is
// src[a] + w * src[b] with (w, a, b) from the butterfly table's column `stage`.
struct StageU { stage: vec4<u32> };
@group(0) @binding(0) var<uniform> S: StageU;
@group(0) @binding(1) var butterflyIn: texture_2d<f32>;
@group(0) @binding(2) var stageSrc: texture_2d<f32>;
@group(0) @binding(3) var stageDst: texture_storage_2d<rgba32float, write>;

@compute @workgroup_size(8, 8)
fn stockham(@builtin(global_invocation_id) gid: vec3<u32>) {
  let dims = textureDimensions(stageSrc);
  if (gid.x >= dims.x || gid.y >= dims.y) { return; }
  let bf = textureLoad(butterflyIn, vec2<i32>(i32(S.stage.x), i32(gid.x)), 0);
  let w = bf.xy;
  let a = textureLoad(stageSrc, vec2<i32>(i32(bf.z), i32(gid.y)), 0);
  let b = textureLoad(stageSrc, vec2<i32>(i32(bf.w), i32(gid.y)), 0);
  let out = a.xy + cmul(w, b.xy);
  textureStore(stageDst, vec2<i32>(gid.xy), vec4<f32>(out, 0.0, 0.0));
}

@group(0) @binding(0) var transposeSrc: texture_2d<f32>;
@group(0) @binding(1) var transposeDst: texture_storage_2d<rgba32float, write>;

@compute @workgroup_size(8, 8)
fn transpose(@builtin(global_invocation_id) gid: vec3<u32>) {
  let dims = textureDimensions(transposeSrc);
  if (gid.x >= dims.x || gid.y >= dims.y) { return; }
  textureStore(transposeDst, vec2<i32>(gid.xy), textureLoad(transposeSrc, vec2<i32>(i32(gid.y), i32(gid.x)), 0));
}

// After the column pass the data is transposed; put it back as field f's result.
@group(0) @binding(0) var fieldSrc: texture_2d<f32>;
@group(0) @binding(1) var fieldDst: texture_storage_2d<rgba32float, write>;

@compute @workgroup_size(8, 8)
fn fieldOut(@builtin(global_invocation_id) gid: vec3<u32>) {
  let dims = textureDimensions(fieldSrc);
  if (gid.x >= dims.x || gid.y >= dims.y) { return; }
  textureStore(fieldDst, vec2<i32>(gid.xy), textureLoad(fieldSrc, vec2<i32>(i32(gid.y), i32(gid.x)), 0));
}

// The cascade maps: undo the centred spectrum's (-1)^(x+y), chop, normal from
// the slopes, Jacobian of the horizontal displacement (folding = J < 0).
@group(0) @binding(0) var<uniform> Pu: EvolveU;
@group(0) @binding(1) var field0: texture_2d<f32>;
@group(0) @binding(2) var field1: texture_2d<f32>;
@group(0) @binding(3) var field2: texture_2d<f32>;
@group(0) @binding(4) var field3: texture_2d<f32>;
@group(0) @binding(5) var displacementOut: texture_storage_2d<rgba16float, write>;
@group(0) @binding(6) var normalOut: texture_storage_2d<rgba16float, write>;
@group(0) @binding(7) var jacobianOut: texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8)
fn post(@builtin(global_invocation_id) gid: vec3<u32>) {
  let n = u32(Pu.grid.x);
  if (gid.x >= n || gid.y >= n) { return; }
  var sign = 1.0;
  if (((gid.x + gid.y) & 1u) == 1u) { sign = -1.0; }
  let p = vec2<i32>(gid.xy);
  let f0 = textureLoad(field0, p, 0).xy * sign;
  let f1 = textureLoad(field1, p, 0).xy * sign;
  let f2 = textureLoad(field2, p, 0).xy * sign;
  let f3 = textureLoad(field3, p, 0).xy * sign;
  let lambda = Pu.wave.z;
  let h = f0.x;
  let Dx = f0.y;
  let Dy = f1.x;
  let dhdx = f1.y;
  let dhdy = f2.x;
  let dDxdx = f2.y;
  let dDydy = f3.x;
  let dDxdy = f3.y;
  let normal = normalize(vec3<f32>(-dhdx, -dhdy, 1.0));
  let jacobian = (1.0 + lambda * dDxdx) * (1.0 + lambda * dDydy) - lambda * lambda * dDxdy * dDxdy;
  textureStore(displacementOut, p, vec4<f32>(lambda * Dx, lambda * Dy, h, jacobian));
  textureStore(normalOut, p, vec4<f32>(normal, 0.0));
  textureStore(jacobianOut, p, vec4<f32>(clamp(1.0 - jacobian, 0.0, 1.0), jacobian, 0.0, 0.0));
}
