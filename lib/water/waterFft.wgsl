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
  band: vec4<f32>,   // x = kLow, y = kHigh, z = cascade index
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
  let kx = 2.0 * PI / U.grid.y * (f32(px) - f32(half));
  let ky = 2.0 * PI / U.grid.y * (f32(py) - f32(half));
  let kLen = length(vec2<f32>(kx, ky));
  if (kLen < 1e-6 || kLen < U.band.x || kLen >= U.band.y) { return vec2<f32>(0.0, 0.0); }
  let g = U.sea.y;
  let depth = U.sea.z;
  let windSpeed = U.grid.z;
  let fetch = U.sea.x;
  let kh = kLen * depth;
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
