#!/usr/bin/env python3
import os
import time

LENGTH = 65536
PASSES = 128
OPERATIONS = LENGTH * PASSES
VALUES = [(index * 17 + 3) % 1009 for index in range(LENGTH)]
PARTICLES = [
    (index % 97, index % 89, index % 83, index % 79,
     index % 73, index % 71, index % 67, index % 61)
    for index in range(LENGTH)
]


def benchmark(name, body):
    start = time.perf_counter_ns()
    checksum = body()
    elapsed = time.perf_counter_ns() - start
    print(f"RESULT,python,{name},{elapsed},{checksum}")


def int_sequential_native():
    checksum = 0
    for _pass in range(PASSES):
        for index in range(LENGTH):
            checksum += VALUES[index]
    return checksum


def int_sequential_checked():
    checksum = 0
    for _pass in range(PASSES):
        for index in range(LENGTH):
            value = VALUES[index] if 0 <= index < len(VALUES) else None
            if value is not None:
                checksum += value
    return checksum


def int_collection():
    checksum = 0
    for _pass in range(PASSES):
        for value in VALUES:
            checksum += value
    return checksum


def int_constant_checked():
    checksum = 0
    index = int(os.environ.get("BENCHMARK_INDEX", "137"))
    for _iteration in range(OPERATIONS):
        value = VALUES[index] if 0 <= index < len(VALUES) else None
        if value is not None:
            checksum += value
    return checksum


def int_strided_checked():
    checksum = 0
    index = 0
    for _iteration in range(OPERATIONS):
        value = VALUES[index] if 0 <= index < len(VALUES) else None
        if value is not None:
            checksum += value
        index = (index + 4) % LENGTH
    return checksum


def int_random_checked():
    checksum = 0
    for iteration in range(OPERATIONS):
        index = (iteration * 48271 + 17) % LENGTH
        value = VALUES[index] if 0 <= index < len(VALUES) else None
        if value is not None:
            checksum += value
    return checksum


def int_mostly_valid_checked():
    checksum = 0
    for iteration in range(OPERATIONS):
        index = LENGTH if iteration % 1000 == 0 else (iteration * 48271 + 17) % LENGTH
        value = VALUES[index] if 0 <= index < len(VALUES) else None
        if value is not None:
            checksum += value
    return checksum


def int_mixed_invalid_checked():
    checksum = 0
    for iteration in range(OPERATIONS):
        index = -1 if iteration % 4 == 0 else (iteration * 48271 + 17) % LENGTH
        value = VALUES[index] if 0 <= index < len(VALUES) else None
        if value is not None:
            checksum += value
    return checksum


def struct_sequential_ref():
    checksum = 0
    for _pass in range(PASSES):
        for index in range(LENGTH):
            particle = PARTICLES[index] if 0 <= index < len(PARTICLES) else None
            if particle is not None:
                checksum += particle[0] + particle[5] + particle[6]
    return checksum


def struct_collection_ref():
    checksum = 0
    for _pass in range(PASSES):
        for particle in PARTICLES:
            checksum += particle[0] + particle[5] + particle[6]
    return checksum


def struct_random_ref():
    checksum = 0
    for iteration in range(OPERATIONS):
        index = (iteration * 48271 + 17) % LENGTH
        particle = PARTICLES[index] if 0 <= index < len(PARTICLES) else None
        if particle is not None:
            checksum += particle[0] + particle[5] + particle[6]
    return checksum


SCENARIOS = [
    ("int_sequential_native", int_sequential_native),
    ("int_sequential_checked", int_sequential_checked),
    ("int_collection", int_collection),
    ("int_constant_checked", int_constant_checked),
    ("int_strided_checked", int_strided_checked),
    ("int_random_checked", int_random_checked),
    ("int_mostly_valid_checked", int_mostly_valid_checked),
    ("int_mixed_invalid_checked", int_mixed_invalid_checked),
    ("struct_sequential_ref", struct_sequential_ref),
    ("struct_collection_ref", struct_collection_ref),
    ("struct_random_ref", struct_random_ref),
]

# Scatter update: the contact-solver-shaped read-modify-write kernel
# (docs/physics-box3d-port-research.md) over plain lists, with an explicit
# bounds check per contact. The world is built before the timer starts.
SCATTER_BODIES = 4096
SCATTER_CONTACTS = 16384
SCATTER_ITERATIONS = 50


def run_scatter():
    vx = [(i % 17) * 0.1 - 0.8 for i in range(SCATTER_BODIES)]
    vy = [(i % 13) * 0.1 - 0.6 for i in range(SCATTER_BODIES)]
    vz = [(i % 11) * 0.1 - 0.5 for i in range(SCATTER_BODIES)]
    inv_mass = [1.0] * SCATTER_BODIES
    body_a, body_b = [], []
    for c in range(SCATTER_CONTACTS):
        a = (c * 7919) % SCATTER_BODIES
        b = (c * 104729 + 13) % SCATTER_BODIES
        if b == a:
            b = (a + 1) % SCATTER_BODIES
        body_a.append(a)
        body_b.append(b)
    nx = [(c % 5) * 0.1 for c in range(SCATTER_CONTACTS)]
    ny = [1.0] * SCATTER_CONTACTS
    nz = [0.0] * SCATTER_CONTACTS
    accumulated = [0.0] * SCATTER_CONTACTS

    def kernel():
        for _iteration in range(SCATTER_ITERATIONS):
            for c in range(SCATTER_CONTACTS):
                a = body_a[c]
                b = body_b[c]
                if not (0 <= a < SCATTER_BODIES and 0 <= b < SCATTER_BODIES):
                    continue
                normal_x, normal_y, normal_z = nx[c], ny[c], nz[c]
                inv_a, inv_b = inv_mass[a], inv_mass[b]
                normal_velocity = ((vx[b] - vx[a]) * normal_x + (vy[b] - vy[a]) * normal_y
                                   + (vz[b] - vz[a]) * normal_z)
                previous = accumulated[c]
                total = previous - normal_velocity / (inv_a + inv_b)
                if total < 0.0:
                    total = 0.0
                impulse = total - previous
                accumulated[c] = total
                vx[a] -= inv_a * impulse * normal_x
                vy[a] -= inv_a * impulse * normal_y
                vz[a] -= inv_a * impulse * normal_z
                vx[b] += inv_b * impulse * normal_x
                vy[b] += inv_b * impulse * normal_y
                vz[b] += inv_b * impulse * normal_z
        total_sum = 0.0
        for i in range(SCATTER_BODIES):
            total_sum = total_sum + vx[i] + vy[i] + vz[i]
        return int(total_sum * 100 - 0.5) if total_sum < 0 else int(total_sum * 100 + 0.5)

    benchmark("scatter_list", kernel)


for _repetition in range(9):
    for scenario_name, scenario in SCENARIOS:
        benchmark(scenario_name, scenario)
    run_scatter()
