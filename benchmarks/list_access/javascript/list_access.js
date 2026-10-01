"use strict";

const length = 65536;
const passes = 128;
const operations = length * passes;
const values = Array.from({length}, (_, index) => (index * 17 + 3) % 1009);
const particles = Array.from({length}, (_, index) => ({
  px: index % 97, py: index % 89, pz: index % 83, vx: index % 79,
  vy: index % 73, vz: index % 71, mass: index % 67, flags: index % 61,
}));

function benchmark(name, body) {
  const start = process.hrtime.bigint();
  const checksum = body();
  const elapsed = process.hrtime.bigint() - start;
  console.log(`RESULT,javascript,${name},${elapsed},${checksum}`);
}

function runSuite() {
  benchmark("int_sequential_native", () => {
    let checksum = 0;
    for (let pass = 0; pass < passes; pass++)
      for (let index = 0; index < length; index++) checksum += values[index];
    return checksum;
  });
  benchmark("int_sequential_checked", () => {
    let checksum = 0;
    for (let pass = 0; pass < passes; pass++) {
      for (let index = 0; index < length; index++) {
        const value = index >= 0 && index < values.length ? values[index] : undefined;
        if (value !== undefined) checksum += value;
      }
    }
    return checksum;
  });
  benchmark("int_collection", () => {
    let checksum = 0;
    for (let pass = 0; pass < passes; pass++)
      for (const value of values) checksum += value;
    return checksum;
  });
  benchmark("int_constant_checked", () => {
    let checksum = 0;
    const index = Number(process.env.BENCHMARK_INDEX || 137);
    for (let iteration = 0; iteration < operations; iteration++) {
      const value = index >= 0 && index < values.length ? values[index] : undefined;
      if (value !== undefined) checksum += value;
    }
    return checksum;
  });
  benchmark("int_strided_checked", () => {
    let checksum = 0;
    let index = 0;
    for (let iteration = 0; iteration < operations; iteration++) {
      const value = index >= 0 && index < values.length ? values[index] : undefined;
      if (value !== undefined) checksum += value;
      index = (index + 4) % length;
    }
    return checksum;
  });
  benchmark("int_random_checked", () => {
    let checksum = 0;
    for (let iteration = 0; iteration < operations; iteration++) {
      const index = (iteration * 48271 + 17) % length;
      const value = index >= 0 && index < values.length ? values[index] : undefined;
      if (value !== undefined) checksum += value;
    }
    return checksum;
  });
  benchmark("int_mostly_valid_checked", () => {
    let checksum = 0;
    for (let iteration = 0; iteration < operations; iteration++) {
      const index = iteration % 1000 === 0 ? length : (iteration * 48271 + 17) % length;
      const value = index >= 0 && index < values.length ? values[index] : undefined;
      if (value !== undefined) checksum += value;
    }
    return checksum;
  });
  benchmark("int_mixed_invalid_checked", () => {
    let checksum = 0;
    for (let iteration = 0; iteration < operations; iteration++) {
      const index = iteration % 4 === 0 ? -1 : (iteration * 48271 + 17) % length;
      const value = index >= 0 && index < values.length ? values[index] : undefined;
      if (value !== undefined) checksum += value;
    }
    return checksum;
  });
  benchmark("struct_sequential_ref", () => {
    let checksum = 0;
    for (let pass = 0; pass < passes; pass++) {
      for (let index = 0; index < length; index++) {
        const particle = index >= 0 && index < particles.length ? particles[index] : undefined;
        if (particle !== undefined) checksum += particle.px + particle.vz + particle.mass;
      }
    }
    return checksum;
  });
  benchmark("struct_collection_ref", () => {
    let checksum = 0;
    for (let pass = 0; pass < passes; pass++)
      for (const particle of particles) checksum += particle.px + particle.vz + particle.mass;
    return checksum;
  });
  benchmark("struct_random_ref", () => {
    let checksum = 0;
    for (let iteration = 0; iteration < operations; iteration++) {
      const index = (iteration * 48271 + 17) % length;
      const particle = index >= 0 && index < particles.length ? particles[index] : undefined;
      if (particle !== undefined) checksum += particle.px + particle.vz + particle.mass;
    }
    return checksum;
  });
}

// Scatter update: the contact-solver-shaped read-modify-write kernel
// (docs/physics-box3d-port-research.md) over Float32Arrays, with an explicit
// bounds check per contact. The world is built before the timer starts.
const SCATTER_BODIES = 4096, SCATTER_CONTACTS = 16384, SCATTER_ITERATIONS = 50;

function runScatter() {
  const vx = new Float32Array(SCATTER_BODIES), vy = new Float32Array(SCATTER_BODIES);
  const vz = new Float32Array(SCATTER_BODIES), invMass = new Float32Array(SCATTER_BODIES).fill(1);
  for (let i = 0; i < SCATTER_BODIES; i++) {
    vx[i] = (i % 17) * 0.1 - 0.8; vy[i] = (i % 13) * 0.1 - 0.6; vz[i] = (i % 11) * 0.1 - 0.5;
  }
  const bodyA = new Int32Array(SCATTER_CONTACTS), bodyB = new Int32Array(SCATTER_CONTACTS);
  const nx = new Float32Array(SCATTER_CONTACTS), ny = new Float32Array(SCATTER_CONTACTS).fill(1);
  const nz = new Float32Array(SCATTER_CONTACTS), accumulated = new Float32Array(SCATTER_CONTACTS);
  for (let c = 0; c < SCATTER_CONTACTS; c++) {
    const a = (c * 7919) % SCATTER_BODIES;
    let b = (c * 104729 + 13) % SCATTER_BODIES;
    if (b === a) b = (a + 1) % SCATTER_BODIES;
    bodyA[c] = a; bodyB[c] = b; nx[c] = (c % 5) * 0.1;
  }
  benchmark("scatter_typed_array", () => {
    for (let iteration = 0; iteration < SCATTER_ITERATIONS; iteration++) {
      for (let c = 0; c < SCATTER_CONTACTS; c++) {
        const a = bodyA[c], b = bodyB[c];
        if (a < 0 || a >= SCATTER_BODIES || b < 0 || b >= SCATTER_BODIES) continue;
        const normalX = nx[c], normalY = ny[c], normalZ = nz[c];
        const invA = invMass[a], invB = invMass[b];
        const normalVelocity = (vx[b] - vx[a]) * normalX + (vy[b] - vy[a]) * normalY + (vz[b] - vz[a]) * normalZ;
        const previous = accumulated[c];
        let total = previous - normalVelocity / (invA + invB);
        if (total < 0) total = 0;
        const lambda = total - previous;
        accumulated[c] = total;
        vx[a] -= invA * lambda * normalX; vy[a] -= invA * lambda * normalY; vz[a] -= invA * lambda * normalZ;
        vx[b] += invB * lambda * normalX; vy[b] += invB * lambda * normalY; vz[b] += invB * lambda * normalZ;
      }
    }
    let sum = 0;
    for (let i = 0; i < SCATTER_BODIES; i++) sum = sum + vx[i] + vy[i] + vz[i];
    return sum < 0 ? Math.trunc(sum * 100 - 0.5) : Math.trunc(sum * 100 + 0.5);
  });
}

for (let repetition = 0; repetition < 9; repetition++) {
  runSuite();
  runScatter();
}
