use std::time::Instant;

#[derive(Clone, Copy)]
struct Particle {
    px: i64, py: i64, pz: i64, vx: i64,
    vy: i64, vz: i64, mass: i64, flags: i64,
}

fn report(name: &str, start: Instant, checksum: i64) {
    println!("RESULT,rust,{},{},{}", name, start.elapsed().as_nanos(), checksum);
}

macro_rules! bench {
    ($name:expr, $checksum:ident, $body:block) => {{
        let start = Instant::now();
        let mut $checksum: i64 = 0;
        $body
        report($name, start, $checksum);
    }};
}

fn run_suite(values: &[i64], particles: &[Particle], passes: i64) {
    let length = values.len() as i64;
    let operations = length * passes;
    // The modulo divisor goes through black_box so `% divisor` is a real
    // division, as it is in Rae, instead of a mask the optimiser derives from
    // the constant 65 536 (it did; it made these cases measure constant folding).
    let divisor = std::hint::black_box(length);
    bench!("int_sequential_safe_index", checksum, {
        for _pass in 0..passes { for index in 0..length { checksum += values[index as usize]; } }
    });
    bench!("int_sequential_get", checksum, {
        for _pass in 0..passes { for index in 0..length { if let Some(value) = values.get(index as usize) { checksum += *value; } } }
    });
    bench!("int_iterator", checksum, {
        for _pass in 0..passes { for value in values { checksum += *value; } }
    });
    bench!("int_unchecked", checksum, {
        for _pass in 0..passes { for index in 0..length { unsafe { checksum += *values.get_unchecked(index as usize); } } }
    });
    bench!("int_constant_get", checksum, {
        let constant_index = 137_usize;
        for _iteration in 0..operations {
            let index = unsafe { std::ptr::read_volatile(&constant_index) };
            if let Some(value) = values.get(index) { checksum += *value; }
        }
    });
    bench!("int_strided_get", checksum, {
        let mut index = 0_i64;
        for _iteration in 0..operations {
            if let Some(value) = values.get(index as usize) { checksum += *value; }
            index = (index + 4) % divisor;
        }
    });
    bench!("int_random_get", checksum, {
        for iteration in 0..operations {
            let index = (iteration * 48271 + 17) % divisor;
            if let Some(value) = values.get(index as usize) { checksum += *value; }
        }
    });
    bench!("int_mostly_valid_get", checksum, {
        for iteration in 0..operations {
            let index = if iteration % 1000 == 0 { length } else { (iteration * 48271 + 17) % divisor };
            if let Some(value) = values.get(index as usize) { checksum += *value; }
        }
    });
    bench!("int_mixed_invalid_get", checksum, {
        for iteration in 0..operations {
            let index = if iteration % 4 == 0 { usize::MAX } else { ((iteration * 48271 + 17) % divisor) as usize };
            if let Some(value) = values.get(index) { checksum += *value; }
        }
    });
    bench!("struct_sequential_copy", checksum, {
        for _pass in 0..passes { for index in 0..length { let particle = particles[index as usize]; checksum += particle.px + particle.vz + particle.mass; } }
    });
    bench!("struct_sequential_get", checksum, {
        for _pass in 0..passes { for index in 0..length { if let Some(particle) = particles.get(index as usize) { checksum += particle.px + particle.vz + particle.mass; } } }
    });
    bench!("struct_iterator_copy", checksum, {
        for _pass in 0..passes { for &particle in particles { checksum += particle.px + particle.vz + particle.mass; } }
    });
    bench!("struct_iterator_ref", checksum, {
        for _pass in 0..passes { for particle in particles { checksum += particle.px + particle.vz + particle.mass; } }
    });
    bench!("struct_random_get", checksum, {
        for iteration in 0..operations {
            let index = ((iteration * 48271 + 17) % divisor) as usize;
            if let Some(particle) = particles.get(index) { checksum += particle.px + particle.vz + particle.mass; }
        }
    });
}

// Scatter update: the contact-solver-shaped read-modify-write kernel
// (docs/physics-box3d-port-research.md), with idiomatic safe `v[i]` indexing
// (bounds-checked, panics on a bad index).
const SCATTER_BODIES: usize = 4096;
const SCATTER_CONTACTS: usize = 16384;
const SCATTER_ITERATIONS: usize = 50;

fn run_scatter() {
    let mut vx: Vec<f32> = (0..SCATTER_BODIES).map(|i| (i % 17) as f32 * 0.1 - 0.8).collect();
    let mut vy: Vec<f32> = (0..SCATTER_BODIES).map(|i| (i % 13) as f32 * 0.1 - 0.6).collect();
    let mut vz: Vec<f32> = (0..SCATTER_BODIES).map(|i| (i % 11) as f32 * 0.1 - 0.5).collect();
    let inv_mass: Vec<f32> = vec![1.0; SCATTER_BODIES];
    let mut body_a: Vec<usize> = Vec::with_capacity(SCATTER_CONTACTS);
    let mut body_b: Vec<usize> = Vec::with_capacity(SCATTER_CONTACTS);
    for c in 0..SCATTER_CONTACTS {
        let a = (c * 7919) % SCATTER_BODIES;
        let mut b = (c * 104729 + 13) % SCATTER_BODIES;
        if b == a { b = (a + 1) % SCATTER_BODIES; }
        body_a.push(a);
        body_b.push(b);
    }
    let nx: Vec<f32> = (0..SCATTER_CONTACTS).map(|c| (c % 5) as f32 * 0.1).collect();
    let ny: Vec<f32> = vec![1.0; SCATTER_CONTACTS];
    let nz: Vec<f32> = vec![0.0; SCATTER_CONTACTS];
    let mut accumulated: Vec<f32> = vec![0.0; SCATTER_CONTACTS];
    let start = Instant::now();
    for _iteration in 0..SCATTER_ITERATIONS {
        for c in 0..SCATTER_CONTACTS {
            let (a, b) = (body_a[c], body_b[c]);
            let (normal_x, normal_y, normal_z) = (nx[c], ny[c], nz[c]);
            let (inv_a, inv_b) = (inv_mass[a], inv_mass[b]);
            let normal_velocity = (vx[b] - vx[a]) * normal_x + (vy[b] - vy[a]) * normal_y
                + (vz[b] - vz[a]) * normal_z;
            let previous = accumulated[c];
            let mut total = previous - normal_velocity / (inv_a + inv_b);
            if total < 0.0 { total = 0.0; }
            let lambda = total - previous;
            accumulated[c] = total;
            vx[a] -= inv_a * lambda * normal_x; vy[a] -= inv_a * lambda * normal_y; vz[a] -= inv_a * lambda * normal_z;
            vx[b] += inv_b * lambda * normal_x; vy[b] += inv_b * lambda * normal_y; vz[b] += inv_b * lambda * normal_z;
        }
    }
    let mut sum: f32 = 0.0;
    for i in 0..SCATTER_BODIES { sum = sum + vx[i] + vy[i] + vz[i]; }
    let checksum = if sum < 0.0 { (sum * 100.0 - 0.5) as i64 } else { (sum * 100.0 + 0.5) as i64 };
    report("scatter_safe_index", start, checksum);
}

fn main() {
    let length = 65536_i64;
    let passes = 128_i64;
    let values: Vec<i64> = (0..length).map(|index| (index * 17 + 3) % 1009).collect();
    let particles: Vec<Particle> = (0..length).map(|index| Particle {
        px: index % 97, py: index % 89, pz: index % 83, vx: index % 79,
        vy: index % 73, vz: index % 71, mass: index % 67, flags: index % 61,
    }).collect();
    let field_guard = particles[0].py + particles[0].pz + particles[0].vx
        + particles[0].vy + particles[0].flags;
    std::hint::black_box(field_guard);
    for _repetition in 0..9 { run_suite(&values, &particles, passes); run_scatter(); }
}
