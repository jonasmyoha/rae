/* The reference for benchmarks/parallel_stages: the same 220-stage step run
 * with Box3D's own synchronisation (solver.c b3SolverTask / b3ExecuteStage):
 * the leader publishes each stage in one atomic word and works too; helpers
 * spin on it, claim blocks with a compare-and-swap on each block's sync index
 * starting from their own offset, and add what they did to the stage's
 * completion counter; the leader spins until the counter is full. "flat" runs
 * the same blocks as one stage. What this costs per stage on a machine is the
 * bar the Rae pool's barrier is measured against. Usage: box3d_style <workers> [rounds] */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { stageCount = 220, blocksPerStage = 32 };
static int rounds = 400;   /* read from argv so the compiler cannot fold the work */

static float g_out[stageCount * blocksPerStage];
static _Atomic int g_sync[stageCount * blocksPerStage];
static _Atomic int g_completion;
static _Atomic uint64_t g_published;   /* (sequence << 32) | stage, 0 = none, ~0 = stop */
static int g_workers;
static int g_flat;                      /* 1: one stage holding every block */

static inline void relax(void) {
#if defined(__aarch64__)
  __asm__ __volatile__("yield" ::: "memory");
#else
  __asm__ __volatile__("pause" ::: "memory");
#endif
}

static float block_work(int seed) {
  float x = (float)(seed % 997) * 0.001f;
  for (int k = 0; k < rounds; k++) x = x * 0.999f + 0.25f;
  return x;
}

static void execute_stage(int stage, int sequence, int worker) {
  int first = g_flat ? 0 : stage * blocksPerStage;
  int count = g_flat ? stageCount * blocksPerStage : blocksPerStage;
  int start = (int)((int64_t)worker * count / g_workers);
  int done = 0;
  for (int i = 0, b = start; i < count; i++) {
    int expected = sequence - 1;
    if (atomic_compare_exchange_strong(&g_sync[first + b], &expected, sequence)) {
      g_out[first + b] = block_work(first + b);
      done++;
    }
    if (++b == count) b = 0;
  }
  atomic_fetch_add(&g_completion, done);
}

static void* helper(void* arg) {
  int worker = (int)(intptr_t)arg;
  uint64_t last = 0;
  for (;;) {
    uint64_t published;
    while ((published = atomic_load(&g_published)) == last) relax();
    if (published == UINT64_MAX) return NULL;
    last = published;
    execute_stage((int)(published & 0xffffffffu), (int)(published >> 32), worker);
  }
}

static int64_t now_ns(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static uint64_t g_sequence = 0;

static void run_step(int flat) {
  g_flat = flat;
  int stages = flat ? 1 : stageCount;
  int count = flat ? stageCount * blocksPerStage : blocksPerStage;
  for (int s = 0; s < stages; s++) {
    g_sequence++;
    /* every block's sync index must equal sequence - 1 before publishing */
    int first = flat ? 0 : s * blocksPerStage;
    for (int b = 0; b < count; b++) atomic_store_explicit(&g_sync[first + b], (int)g_sequence - 1, memory_order_relaxed);
    atomic_store(&g_completion, 0);
    atomic_store(&g_published, (g_sequence << 32) | (uint64_t)s);
    execute_stage(s, (int)g_sequence, 0);
    while (atomic_load(&g_completion) != count) relax();
  }
}

int main(int argc, char** argv) {
  g_workers = argc > 1 ? atoi(argv[1]) : 1;
  if (argc > 2) rounds = atoi(argv[2]);
  if (g_workers < 1) g_workers = 1;
  pthread_t threads[64];
  for (int w = 1; w < g_workers; w++) pthread_create(&threads[w], NULL, helper, (void*)(intptr_t)w);
  int64_t best_staged = 0, best_flat = 0;
  for (int run = 0; run < 7; run++) {
    int64_t t0 = now_ns(); run_step(0); int64_t staged = now_ns() - t0;
    t0 = now_ns(); run_step(1); int64_t flat = now_ns() - t0;
    if (run == 0 || staged < best_staged) best_staged = staged;
    if (run == 0 || flat < best_flat) best_flat = flat;
  }
  atomic_store(&g_published, UINT64_MAX);
  for (int w = 1; w < g_workers; w++) pthread_join(threads[w], NULL);
  double checksum = 0.0;   /* read the results, or the compiler drops the work */
  for (int i = 0; i < stageCount * blocksPerStage; i++) checksum += g_out[i];
  printf("RESULT,%d,%lld,%lld,%.3f\n", g_workers, (long long)best_staged, (long long)best_flat, checksum);
  return 0;
}
