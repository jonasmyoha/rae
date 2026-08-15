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

for _repetition in range(9):
    for scenario_name, scenario in SCENARIOS:
        benchmark(scenario_name, scenario)
