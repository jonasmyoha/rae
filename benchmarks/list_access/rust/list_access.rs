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
            index = (index + 4) % length;
        }
    });
    bench!("int_random_get", checksum, {
        for iteration in 0..operations {
            let index = (iteration * 48271 + 17) % length;
            if let Some(value) = values.get(index as usize) { checksum += *value; }
        }
    });
    bench!("int_mostly_valid_get", checksum, {
        for iteration in 0..operations {
            let index = if iteration % 1000 == 0 { length } else { (iteration * 48271 + 17) % length };
            if let Some(value) = values.get(index as usize) { checksum += *value; }
        }
    });
    bench!("int_mixed_invalid_get", checksum, {
        for iteration in 0..operations {
            let index = if iteration % 4 == 0 { usize::MAX } else { ((iteration * 48271 + 17) % length) as usize };
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
            let index = ((iteration * 48271 + 17) % length) as usize;
            if let Some(particle) = particles.get(index) { checksum += particle.px + particle.vz + particle.mass; }
        }
    });
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
    for _repetition in 0..9 { run_suite(&values, &particles, passes); }
}
