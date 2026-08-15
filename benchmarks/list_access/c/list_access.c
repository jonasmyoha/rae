#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
  int64_t px, py, pz, vx, vy, vz, mass, flags;
} Particle;

static int64_t now_ns(void) {
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return (int64_t)value.tv_sec * 1000000000LL + value.tv_nsec;
}

static void report(const char* name, int64_t start, int64_t checksum) {
  printf("RESULT,c,%s,%lld,%lld\n", name,
         (long long)(now_ns() - start), (long long)checksum);
}

#define RUN_INT(NAME, BODY) do { \
  int64_t start = now_ns(), checksum = 0; \
  BODY \
  report(NAME, start, checksum); \
} while (0)

#define RUN_STRUCT(NAME, BODY) RUN_INT(NAME, BODY)

static void run_suite(const int64_t* values, const Particle* particles,
                      int64_t length, int64_t passes) {
  const int64_t operations = length * passes;
  RUN_INT("int_sequential_unchecked", {
    for (int64_t pass = 0; pass < passes; pass++)
      for (int64_t index = 0; index < length; index++) checksum += values[index];
  });
  RUN_INT("int_sequential_checked", {
    for (int64_t pass = 0; pass < passes; pass++)
      for (int64_t index = 0; index < length; index++)
        if ((uint64_t)index < (uint64_t)length) checksum += values[index];
  });
  RUN_INT("int_collection", {
    for (int64_t pass = 0; pass < passes; pass++) {
      const int64_t* end = values + length;
      for (const int64_t* value = values; value != end; value++) checksum += *value;
    }
  });
  RUN_INT("int_constant_checked", {
    volatile int64_t constant_index = 137;
    for (int64_t iteration = 0; iteration < operations; iteration++)
      if (constant_index < length) checksum += values[constant_index];
  });
  RUN_INT("int_strided_checked", {
    int64_t index = 0;
    for (int64_t iteration = 0; iteration < operations; iteration++) {
      if ((uint64_t)index < (uint64_t)length) checksum += values[index];
      index = (index + 4) % length;
    }
  });
  RUN_INT("int_random_checked", {
    for (int64_t iteration = 0; iteration < operations; iteration++) {
      int64_t index = (iteration * 48271 + 17) % length;
      if ((uint64_t)index < (uint64_t)length) checksum += values[index];
    }
  });
  RUN_INT("int_mostly_valid_checked", {
    for (int64_t iteration = 0; iteration < operations; iteration++) {
      int64_t index = iteration % 1000 == 0 ? length : (iteration * 48271 + 17) % length;
      if ((uint64_t)index < (uint64_t)length) checksum += values[index];
    }
  });
  RUN_INT("int_mixed_invalid_checked", {
    for (int64_t iteration = 0; iteration < operations; iteration++) {
      int64_t index = iteration % 4 == 0 ? -1 : (iteration * 48271 + 17) % length;
      if ((uint64_t)index < (uint64_t)length) checksum += values[index];
    }
  });
  RUN_STRUCT("struct_sequential_value", {
    for (int64_t pass = 0; pass < passes; pass++)
      for (int64_t index = 0; index < length; index++) {
        Particle particle = particles[index];
        checksum += particle.px + particle.vz + particle.mass;
      }
  });
  RUN_STRUCT("struct_sequential_pointer", {
    for (int64_t pass = 0; pass < passes; pass++)
      for (int64_t index = 0; index < length; index++) {
        const Particle* particle = &particles[index];
        checksum += particle->px + particle->vz + particle->mass;
      }
  });
  RUN_STRUCT("struct_collection_value", {
    for (int64_t pass = 0; pass < passes; pass++)
      for (int64_t index = 0; index < length; index++) {
        Particle particle = particles[index];
        checksum += particle.px + particle.vz + particle.mass;
      }
  });
  RUN_STRUCT("struct_collection_pointer", {
    for (int64_t pass = 0; pass < passes; pass++)
      for (const Particle* particle = particles; particle != particles + length; particle++)
        checksum += particle->px + particle->vz + particle->mass;
  });
  RUN_STRUCT("struct_random_pointer", {
    for (int64_t iteration = 0; iteration < operations; iteration++) {
      int64_t index = (iteration * 48271 + 17) % length;
      const Particle* particle = &particles[index];
      checksum += particle->px + particle->vz + particle->mass;
    }
  });
}

int main(void) {
  const int64_t length = 65536, passes = 128;
  int64_t* values = malloc((size_t)length * sizeof(*values));
  Particle* particles = malloc((size_t)length * sizeof(*particles));
  for (int64_t index = 0; index < length; index++) {
    values[index] = (index * 17 + 3) % 1009;
    particles[index] = (Particle){index % 97, index % 89, index % 83,
      index % 79, index % 73, index % 71, index % 67, index % 61};
  }
  for (int repetition = 0; repetition < 9; repetition++)
    run_suite(values, particles, length, passes);
  free(particles);
  free(values);
  return 0;
}
