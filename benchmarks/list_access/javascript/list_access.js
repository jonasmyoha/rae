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

for (let repetition = 0; repetition < 9; repetition++) runSuite();
