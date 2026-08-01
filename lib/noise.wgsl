// Shared procedural noise for Rae WebGPU shaders.
// Keep the hash constants and algorithms aligned with lib/noise.rae.
// Hash/value return [0,1]; Perlin/simplex/normalized FBM are approximately
// [-1,1]. Prefixes avoid collisions when this prelude is composed with apps.

fn raeNoiseHashWord(value0: u32) -> u32 {
  var value = value0;
  value = value ^ (value >> 16u);
  value = value * 0x7feb352du;
  value = value ^ (value >> 15u);
  value = value * 0x846ca68bu;
  value = value ^ (value >> 16u);
  return value;
}

fn raeNoiseHashLattice2(p: vec2<i32>, seed: u32) -> u32 {
  var h = seed ^ 0x9e3779b9u;
  h = raeNoiseHashWord(h ^ (bitcast<u32>(p.x) * 0x85ebca6bu));
  h = raeNoiseHashWord(h ^ (bitcast<u32>(p.y) * 0xc2b2ae35u));
  return h;
}

fn raeNoiseHashLattice3(p: vec3<i32>, seed: u32) -> u32 {
  var h = raeNoiseHashLattice2(p.xy, seed);
  h = raeNoiseHashWord(h ^ (bitcast<u32>(p.z) * 0x27d4eb2fu));
  return h;
}

fn raeNoiseHash2(p: vec2<i32>, seed: u32) -> f32 {
  return f32(raeNoiseHashLattice2(p, seed)) / 4294967295.0;
}

fn raeNoiseHash3(p: vec3<i32>, seed: u32) -> f32 {
  return f32(raeNoiseHashLattice3(p, seed)) / 4294967295.0;
}

fn raeNoiseFade(t: f32) -> f32 {
  return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

fn raeNoiseValue2(p: vec2<f32>, seed: u32) -> f32 {
  let cell = vec2<i32>(floor(p));
  let f = fract(p);
  let u = vec2<f32>(raeNoiseFade(f.x), raeNoiseFade(f.y));
  let a = mix(raeNoiseHash2(cell, seed), raeNoiseHash2(cell + vec2<i32>(1, 0), seed), u.x);
  let b = mix(raeNoiseHash2(cell + vec2<i32>(0, 1), seed), raeNoiseHash2(cell + vec2<i32>(1, 1), seed), u.x);
  return mix(a, b, u.y);
}

fn raeNoiseValue3(p: vec3<f32>, seed: u32) -> f32 {
  let cell = vec3<i32>(floor(p));
  let f = fract(p);
  let u = vec3<f32>(raeNoiseFade(f.x), raeNoiseFade(f.y), raeNoiseFade(f.z));
  let x00 = mix(raeNoiseHash3(cell, seed), raeNoiseHash3(cell + vec3<i32>(1, 0, 0), seed), u.x);
  let x10 = mix(raeNoiseHash3(cell + vec3<i32>(0, 1, 0), seed), raeNoiseHash3(cell + vec3<i32>(1, 1, 0), seed), u.x);
  let x01 = mix(raeNoiseHash3(cell + vec3<i32>(0, 0, 1), seed), raeNoiseHash3(cell + vec3<i32>(1, 0, 1), seed), u.x);
  let x11 = mix(raeNoiseHash3(cell + vec3<i32>(0, 1, 1), seed), raeNoiseHash3(cell + vec3<i32>(1, 1, 1), seed), u.x);
  return mix(mix(x00, x10, u.y), mix(x01, x11, u.y), u.z);
}

fn raeNoiseGrad2(hash: u32, p: vec2<f32>) -> f32 {
  let h = hash & 7u;
  if (h == 0u) { return p.x; }
  if (h == 1u) { return -p.x; }
  if (h == 2u) { return p.y; }
  if (h == 3u) { return -p.y; }
  if (h == 4u) { return (p.x + p.y) * 0.7071067812; }
  if (h == 5u) { return (-p.x + p.y) * 0.7071067812; }
  if (h == 6u) { return (p.x - p.y) * 0.7071067812; }
  return (-p.x - p.y) * 0.7071067812;
}

fn raeNoiseGrad3(hash: u32, p: vec3<f32>) -> f32 {
  let h = hash % 12u;
  if (h == 0u) { return p.x + p.y; }
  if (h == 1u) { return -p.x + p.y; }
  if (h == 2u) { return p.x - p.y; }
  if (h == 3u) { return -p.x - p.y; }
  if (h == 4u) { return p.x + p.z; }
  if (h == 5u) { return -p.x + p.z; }
  if (h == 6u) { return p.x - p.z; }
  if (h == 7u) { return -p.x - p.z; }
  if (h == 8u) { return p.y + p.z; }
  if (h == 9u) { return -p.y + p.z; }
  if (h == 10u) { return p.y - p.z; }
  return -p.y - p.z;
}

fn raeNoisePerlin2(p: vec2<f32>, seed: u32) -> f32 {
  let cell = vec2<i32>(floor(p));
  let f = fract(p);
  let u = vec2<f32>(raeNoiseFade(f.x), raeNoiseFade(f.y));
  let a = mix(raeNoiseGrad2(raeNoiseHashLattice2(cell, seed), f), raeNoiseGrad2(raeNoiseHashLattice2(cell + vec2<i32>(1, 0), seed), f - vec2<f32>(1.0, 0.0)), u.x);
  let b = mix(raeNoiseGrad2(raeNoiseHashLattice2(cell + vec2<i32>(0, 1), seed), f - vec2<f32>(0.0, 1.0)), raeNoiseGrad2(raeNoiseHashLattice2(cell + vec2<i32>(1, 1), seed), f - vec2<f32>(1.0, 1.0)), u.x);
  return mix(a, b, u.y) * 1.4142135624;
}

fn raeNoisePerlin3(p: vec3<f32>, seed: u32) -> f32 {
  let cell = vec3<i32>(floor(p));
  let f = fract(p);
  let u = vec3<f32>(raeNoiseFade(f.x), raeNoiseFade(f.y), raeNoiseFade(f.z));
  let x00 = mix(raeNoiseGrad3(raeNoiseHashLattice3(cell, seed), f), raeNoiseGrad3(raeNoiseHashLattice3(cell + vec3<i32>(1, 0, 0), seed), f - vec3<f32>(1.0, 0.0, 0.0)), u.x);
  let x10 = mix(raeNoiseGrad3(raeNoiseHashLattice3(cell + vec3<i32>(0, 1, 0), seed), f - vec3<f32>(0.0, 1.0, 0.0)), raeNoiseGrad3(raeNoiseHashLattice3(cell + vec3<i32>(1, 1, 0), seed), f - vec3<f32>(1.0, 1.0, 0.0)), u.x);
  let x01 = mix(raeNoiseGrad3(raeNoiseHashLattice3(cell + vec3<i32>(0, 0, 1), seed), f - vec3<f32>(0.0, 0.0, 1.0)), raeNoiseGrad3(raeNoiseHashLattice3(cell + vec3<i32>(1, 0, 1), seed), f - vec3<f32>(1.0, 0.0, 1.0)), u.x);
  let x11 = mix(raeNoiseGrad3(raeNoiseHashLattice3(cell + vec3<i32>(0, 1, 1), seed), f - vec3<f32>(0.0, 1.0, 1.0)), raeNoiseGrad3(raeNoiseHashLattice3(cell + vec3<i32>(1, 1, 1), seed), f - vec3<f32>(1.0, 1.0, 1.0)), u.x);
  return mix(mix(x00, x10, u.y), mix(x01, x11, u.y), u.z);
}

fn raeNoiseSimplexCorner2(hash: u32, p: vec2<f32>) -> f32 {
  var t = 0.5 - dot(p, p);
  if (t <= 0.0) { return 0.0; }
  t = t * t;
  return t * t * raeNoiseGrad2(hash, p);
}

fn raeNoiseSimplex2(p: vec2<f32>, seed: u32) -> f32 {
  let f2 = 0.3660254038;
  let g2 = 0.2113248654;
  let cell = vec2<i32>(floor(p + dot(p, vec2<f32>(f2))));
  let unskew = f32(cell.x + cell.y) * g2;
  let p0 = p - (vec2<f32>(cell) - vec2<f32>(unskew));
  var offset1 = vec2<i32>(0, 1);
  if (p0.x > p0.y) { offset1 = vec2<i32>(1, 0); }
  let p1 = p0 - vec2<f32>(offset1) + vec2<f32>(g2);
  let p2 = p0 - vec2<f32>(1.0) + vec2<f32>(2.0 * g2);
  let n0 = raeNoiseSimplexCorner2(raeNoiseHashLattice2(cell, seed), p0);
  let n1 = raeNoiseSimplexCorner2(raeNoiseHashLattice2(cell + offset1, seed), p1);
  let n2 = raeNoiseSimplexCorner2(raeNoiseHashLattice2(cell + vec2<i32>(1), seed), p2);
  return 70.0 * (n0 + n1 + n2);
}

fn raeNoiseSimplexCorner3(hash: u32, p: vec3<f32>) -> f32 {
  var t = 0.6 - dot(p, p);
  if (t <= 0.0) { return 0.0; }
  t = t * t;
  return t * t * raeNoiseGrad3(hash, p);
}

fn raeNoiseSimplex3(p: vec3<f32>, seed: u32) -> f32 {
  let skew = (p.x + p.y + p.z) / 3.0;
  let cell = vec3<i32>(floor(p + vec3<f32>(skew)));
  let unskew = f32(cell.x + cell.y + cell.z) / 6.0;
  let p0 = p - (vec3<f32>(cell) - vec3<f32>(unskew));
  var offset1 = vec3<i32>(0);
  var offset2 = vec3<i32>(0);
  if (p0.x >= p0.y) {
    if (p0.y >= p0.z) { offset1 = vec3<i32>(1, 0, 0); offset2 = vec3<i32>(1, 1, 0); }
    else if (p0.x >= p0.z) { offset1 = vec3<i32>(1, 0, 0); offset2 = vec3<i32>(1, 0, 1); }
    else { offset1 = vec3<i32>(0, 0, 1); offset2 = vec3<i32>(1, 0, 1); }
  } else {
    if (p0.y < p0.z) { offset1 = vec3<i32>(0, 0, 1); offset2 = vec3<i32>(0, 1, 1); }
    else if (p0.x < p0.z) { offset1 = vec3<i32>(0, 1, 0); offset2 = vec3<i32>(0, 1, 1); }
    else { offset1 = vec3<i32>(0, 1, 0); offset2 = vec3<i32>(1, 1, 0); }
  }
  let p1 = p0 - vec3<f32>(offset1) + vec3<f32>(1.0 / 6.0);
  let p2 = p0 - vec3<f32>(offset2) + vec3<f32>(1.0 / 3.0);
  let p3 = p0 - vec3<f32>(0.5);
  let n0 = raeNoiseSimplexCorner3(raeNoiseHashLattice3(cell, seed), p0);
  let n1 = raeNoiseSimplexCorner3(raeNoiseHashLattice3(cell + offset1, seed), p1);
  let n2 = raeNoiseSimplexCorner3(raeNoiseHashLattice3(cell + offset2, seed), p2);
  let n3 = raeNoiseSimplexCorner3(raeNoiseHashLattice3(cell + vec3<i32>(1), seed), p3);
  return 32.0 * (n0 + n1 + n2 + n3);
}

fn raeNoiseFbmValue2(p0: vec2<f32>, octaves: u32, lacunarity: f32, gain: f32, seed: u32) -> f32 {
  var p = p0;
  var amplitude = 1.0;
  var sum = 0.0;
  var weight = 0.0;
  for (var octave = 0u; octave < octaves; octave = octave + 1u) {
    sum = sum + raeNoiseValue2(p, seed + octave * 1013u) * amplitude;
    weight = weight + amplitude;
    p = mat2x2<f32>(0.8, 0.6, -0.6, 0.8) * p * lacunarity + vec2<f32>(17.17, 31.31);
    amplitude = amplitude * gain;
  }
  return select(0.0, sum / weight, weight > 0.0);
}

fn raeNoiseFbmValue3(p0: vec3<f32>, octaves: u32, lacunarity: f32, gain: f32, seed: u32) -> f32 {
  var p = p0;
  var amplitude = 1.0;
  var sum = 0.0;
  var weight = 0.0;
  for (var octave = 0u; octave < octaves; octave = octave + 1u) {
    sum = sum + raeNoiseValue3(p, seed + octave * 1013u) * amplitude;
    weight = weight + amplitude;
    p = p * lacunarity + vec3<f32>(17.17, 31.31, 47.47);
    amplitude = amplitude * gain;
  }
  return select(0.0, sum / weight, weight > 0.0);
}

fn raeNoiseFbm2(p0: vec2<f32>, octaves: u32, lacunarity: f32, gain: f32, seed: u32) -> f32 {
  var p = p0;
  var amplitude = 1.0;
  var sum = 0.0;
  var weight = 0.0;
  for (var octave = 0u; octave < octaves; octave = octave + 1u) {
    sum = sum + raeNoiseSimplex2(p, seed + octave * 1013u) * amplitude;
    weight = weight + amplitude;
    p = mat2x2<f32>(0.8, 0.6, -0.6, 0.8) * p * lacunarity + vec2<f32>(17.17, 31.31);
    amplitude = amplitude * gain;
  }
  return select(0.0, sum / weight, weight > 0.0);
}

fn raeNoiseFbm3(p0: vec3<f32>, octaves: u32, lacunarity: f32, gain: f32, seed: u32) -> f32 {
  var p = p0;
  var amplitude = 1.0;
  var sum = 0.0;
  var weight = 0.0;
  for (var octave = 0u; octave < octaves; octave = octave + 1u) {
    sum = sum + raeNoiseSimplex3(p, seed + octave * 1013u) * amplitude;
    weight = weight + amplitude;
    p = vec3<f32>(
      (p.x * 0.8 - p.y * 0.6) * lacunarity + 17.17,
      (p.x * 0.6 + p.y * 0.8) * lacunarity + 31.31,
      (p.z * 0.91 + p.x * 0.27) * lacunarity + 47.47);
    amplitude = amplitude * gain;
  }
  return select(0.0, sum / weight, weight > 0.0);
}

// xy are warped coordinates; z is the sampled value.
fn raeNoiseDomainWarp2(p: vec2<f32>, octaves: u32, lacunarity: f32, gain: f32, strength: f32, seed: u32) -> vec3<f32> {
  let q = vec2<f32>(raeNoiseFbm2(p, octaves, lacunarity, gain, seed), raeNoiseFbm2(p + vec2<f32>(5.2, 1.3), octaves, lacunarity, gain, seed + 101u));
  let warped = p + q * strength;
  return vec3<f32>(warped, raeNoiseFbm2(warped, octaves, lacunarity, gain, seed + 211u));
}

// xyz are warped coordinates; w is the sampled value.
fn raeNoiseDomainWarp3(p: vec3<f32>, octaves: u32, lacunarity: f32, gain: f32, strength: f32, seed: u32) -> vec4<f32> {
  let q = vec3<f32>(
    raeNoiseFbm3(p, octaves, lacunarity, gain, seed),
    raeNoiseFbm3(p + vec3<f32>(5.2, 1.3, 7.1), octaves, lacunarity, gain, seed + 101u),
    raeNoiseFbm3(p + vec3<f32>(8.3, 2.8, 3.4), octaves, lacunarity, gain, seed + 211u));
  let warped = p + q * strength;
  return vec4<f32>(warped, raeNoiseFbm3(warped, octaves, lacunarity, gain, seed + 307u));
}
