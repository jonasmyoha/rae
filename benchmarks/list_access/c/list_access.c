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
  /* The modulo divisor is read through a volatile so `% divisor` is a real
   * division, as it is in Rae, instead of a mask the optimiser derives from the
   * constant 65 536 (it did; it made these cases measure constant folding). */
  volatile int64_t divisor_slot = length;
  const int64_t divisor = divisor_slot;
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
      index = (index + 4) % divisor;
    }
  });
  RUN_INT("int_random_checked", {
    for (int64_t iteration = 0; iteration < operations; iteration++) {
      int64_t index = (iteration * 48271 + 17) % divisor;
      if ((uint64_t)index < (uint64_t)length) checksum += values[index];
    }
  });
  RUN_INT("int_mostly_valid_checked", {
    for (int64_t iteration = 0; iteration < operations; iteration++) {
      int64_t index = iteration % 1000 == 0 ? length : (iteration * 48271 + 17) % divisor;
      if ((uint64_t)index < (uint64_t)length) checksum += values[index];
    }
  });
  RUN_INT("int_mixed_invalid_checked", {
    for (int64_t iteration = 0; iteration < operations; iteration++) {
      int64_t index = iteration % 4 == 0 ? -1 : (iteration * 48271 + 17) % divisor;
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
      int64_t index = (iteration * 48271 + 17) % divisor;
      const Particle* particle = &particles[index];
      checksum += particle->px + particle->vz + particle->mass;
    }
  });
}

/* Scatter update: the contact-solver-shaped read-modify-write kernel
 * (docs/physics-box3d-port-research.md), with explicit bounds checks on every
 * index so it is the same safe access the Rae versions perform. */
enum { SCATTER_BODIES = 4096, SCATTER_CONTACTS = 16384, SCATTER_ITERATIONS = 50 };

static int64_t scatter_round(float sum) {
  return sum < 0.0f ? (int64_t)(sum * 100.0f - 0.5f) : (int64_t)(sum * 100.0f + 0.5f);
}

static void run_scatter(void) {
  static float vx[SCATTER_BODIES], vy[SCATTER_BODIES], vz[SCATTER_BODIES], inv_mass[SCATTER_BODIES];
  static int64_t body_a[SCATTER_CONTACTS], body_b[SCATTER_CONTACTS];
  static float nx[SCATTER_CONTACTS], ny[SCATTER_CONTACTS], nz[SCATTER_CONTACTS], accumulated[SCATTER_CONTACTS];
  for (int64_t i = 0; i < SCATTER_BODIES; i++) {
    vx[i] = (float)(i % 17) * 0.1f - 0.8f;
    vy[i] = (float)(i % 13) * 0.1f - 0.6f;
    vz[i] = (float)(i % 11) * 0.1f - 0.5f;
    inv_mass[i] = 1.0f;
  }
  for (int64_t c = 0; c < SCATTER_CONTACTS; c++) {
    int64_t a = (c * 7919) % SCATTER_BODIES, b = (c * 104729 + 13) % SCATTER_BODIES;
    if (b == a) b = (a + 1) % SCATTER_BODIES;
    body_a[c] = a; body_b[c] = b;
    nx[c] = (float)(c % 5) * 0.1f; ny[c] = 1.0f; nz[c] = 0.0f; accumulated[c] = 0.0f;
  }
  int64_t start = now_ns();
  for (int iteration = 0; iteration < SCATTER_ITERATIONS; iteration++) {
    for (int64_t c = 0; c < SCATTER_CONTACTS; c++) {
      int64_t a = body_a[c], b = body_b[c];
      if ((uint64_t)a >= SCATTER_BODIES || (uint64_t)b >= SCATTER_BODIES) continue;
      float normal_x = nx[c], normal_y = ny[c], normal_z = nz[c];
      float inv_a = inv_mass[a], inv_b = inv_mass[b];
      float normal_velocity = (vx[b] - vx[a]) * normal_x + (vy[b] - vy[a]) * normal_y
                            + (vz[b] - vz[a]) * normal_z;
      float previous = accumulated[c];
      float total = previous - normal_velocity / (inv_a + inv_b);
      if (total < 0.0f) total = 0.0f;
      float lambda = total - previous;
      accumulated[c] = total;
      vx[a] -= inv_a * lambda * normal_x; vy[a] -= inv_a * lambda * normal_y; vz[a] -= inv_a * lambda * normal_z;
      vx[b] += inv_b * lambda * normal_x; vy[b] += inv_b * lambda * normal_y; vz[b] += inv_b * lambda * normal_z;
    }
  }
  float sum = 0.0f;
  for (int64_t i = 0; i < SCATTER_BODIES; i++) sum = sum + vx[i] + vy[i] + vz[i];
  report("scatter_checked", start, scatter_round(sum));
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
  for (int repetition = 0; repetition < 9; repetition++) {
    run_suite(values, particles, length, passes);
    run_scatter();
  }
  free(particles);
  free(values);
  return 0;
}
