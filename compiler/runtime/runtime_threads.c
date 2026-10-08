/* Task and channel runtime primitives. Permanent C kernel for OS-thread and mutex-backed concurrency; higher task/channel policy can migrate to Rae later.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* ----- Task(T) runtime (compiled backend) ----------------------------- */
/* A spawned task gets one RaeTask + one OS thread. The per-spawned-function
 * thunk (emitted by the C backend) runs the function and stores its result
 * into `result`; `rae_task_await` joins exactly once and hands back the
 * buffer, which the get() call site casts to T. */
RaeTask* rae_task_new(size_t result_size) {
  RaeTask* t = (RaeTask*)malloc(sizeof(RaeTask));
  t->result = result_size ? malloc(result_size) : NULL;
  atomic_init(&t->done, 0);
  t->joined = 0;
  t->taken = 0;
  return t;
}

/* Start a spawned task's thunk on its own thread. When no thread can be had
 * (pthread_create fails: macOS stops at 16 383 live threads per process, or
 * the test-only cap RAE_SPAWN_THREAD_CAP=N is reached) the thunk runs right
 * here on the caller, exactly as an uncapturable spawn runs, so the task
 * still computes its result and `get()` returns it. It used to be treated as
 * started, and get() handed back an uncomputed result (docs/lightweight-spawn
 * -design.md §10). The first such fallback is reported once on stderr. */
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
typedef struct {
  void* (*thunk)(void*);
  void* args;
} RaeTaskLaunch;

static _Atomic int64_t g_rae_live_task_threads = 0;
static _Atomic int64_t g_rae_spawn_thread_cap = -2;  /* -2: not read yet; -1: none */
static _Atomic int g_rae_spawn_fallback_reported = 0;

static void* rae_task_trampoline(void* launch_pointer) {
  RaeTaskLaunch launch = *(RaeTaskLaunch*)launch_pointer;
  free(launch_pointer);
  void* answer = launch.thunk(launch.args);
  atomic_fetch_sub_explicit(&g_rae_live_task_threads, 1, memory_order_relaxed);
  return answer;
}

static int64_t rae_spawn_thread_cap(void) {
  int64_t cap = atomic_load_explicit(&g_rae_spawn_thread_cap, memory_order_relaxed);
  if (cap == -2) {
    const char* text = getenv("RAE_SPAWN_THREAD_CAP");
    cap = (text && text[0]) ? (int64_t)atoll(text) : -1;
    atomic_store_explicit(&g_rae_spawn_thread_cap, cap, memory_order_relaxed);
  }
  return cap;
}
#endif

void rae_task_start(RaeTask* t, void* (*thunk)(void*), void* args) {
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  int64_t cap = rae_spawn_thread_cap();
  int64_t live = atomic_fetch_add_explicit(&g_rae_live_task_threads, 1, memory_order_relaxed);
  int error = 0;
  if (cap >= 0 && live >= cap) {
    error = -1;
  } else {
    RaeTaskLaunch* launch = (RaeTaskLaunch*)malloc(sizeof(RaeTaskLaunch));
    if (launch) {
      launch->thunk = thunk;
      launch->args = args;
      error = pthread_create(&t->thread, NULL, rae_task_trampoline, launch);
      if (error == 0) return;
      free(launch);
    } else {
      error = ENOMEM;
    }
  }
  atomic_fetch_sub_explicit(&g_rae_live_task_threads, 1, memory_order_relaxed);
  if (atomic_exchange_explicit(&g_rae_spawn_fallback_reported, 1, memory_order_relaxed) == 0) {
    if (error == -1)
      fprintf(stderr, "[rae] spawn: the thread cap RAE_SPAWN_THREAD_CAP=%lld is reached; tasks beyond it run on the caller "
                      "(shown once)\n", (long long)cap);
    else
      fprintf(stderr, "[rae] spawn: no thread could be started (%s, %lld live); tasks run on the caller until one can "
                      "(shown once)\n", strerror(error), (long long)live);
  }
#endif
  /* The task's result belongs to the task, not to the statement that spawned
   * it: what the thunk leaves in this thread's String pool is released, not
   * flushed (a worker thread's pool is never flushed either) */
  int pool_mark = rae_string_pool_mark();
  thunk(args);
  rae_string_pool_release(pool_mark);
  t->joined = 1;  /* nothing to join: the task ran here */
}

void* rae_task_await(RaeTask* t) {
  if (!t) return NULL;
  if (!t->joined) {
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
    pthread_join(t->thread, NULL);
#endif
    t->joined = 1;
  }
  return t->result;
}

/* Scope-exit drop (join-on-drop): a Task local that goes out of scope is
 * joined so its worker can't outlive the scope (and isn't killed mid-run at
 * process teardown), then freed. NOTE: the result buffer's own contents are
 * not cascade-dropped here — a heap result that was never get()'d leaks its
 * payload; acceptable for now (callers normally get() the result). */
void rae_task_drop(RaeTask* t) {
  if (!t) return;
  if (!t->joined) {
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
    pthread_join(t->thread, NULL);
#endif
    t->joined = 1;
  }
  free(t->result);
  free(t);
}

/* ----- Channel(T) runtime: MPSC ring of T-sized slots (#271, #969) ------ */
/* Backs lib/Channel.rae's `Channel(T)`, Rae's first message-passing
 * primitive. Multi-producer, single-consumer: any thread may send; only the
 * owning consumer thread drains. A pthread mutex serialises every access and
 * is hidden entirely inside the runtime — ordinary Rae code never locks.
 *
 * #969: the ring is a byte array of `cap * elem_size`, so ANY Rae value type
 * is a payload — the element size comes from `sizeof(T)` on the Rae side, and
 * the Rae side does the typed store/load (`rae_ext_rae_buf_set/get`, the same
 * per-T specialisation List uses) while this kernel only hands out slot
 * INDICES under the lock:
 *   reserve -> (Rae stores into slot) -> commit      one producer step
 *   take    -> (Rae copies out of slot) -> release   the consumer step
 * The lock is held across the Rae store/copy, which is a memcpy of T. A full
 * ring refuses the send (reserve = -1); an empty ring refuses the take. The
 * consumer moves the bytes out, so the ring never owns a live T after
 * `release`; `freeChannel` drains what is left (dropping each T in Rae)
 * before this frees the storage. `recv_count` is the monotonic read-only
 * observable of consumer progress. Guarded out for single-threaded wasm,
 * matching RaeTask above. */
#define RAE_CHAN_CAP 4096
typedef struct {
  pthread_mutex_t mu;
  uint8_t* buf;        /* cap * elem_size bytes */
  int64_t elem_size;
  int64_t cap;
  int64_t head;        /* next index to pop */
  int64_t count;       /* items currently buffered */
  int64_t recv_count;  /* total drained (read-only observable) */
} RaeChannel;

#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
#define RAE_CHAN_LOCK(c)   pthread_mutex_lock(&(c)->mu)
#define RAE_CHAN_UNLOCK(c) pthread_mutex_unlock(&(c)->mu)
#else
#define RAE_CHAN_LOCK(c)   ((void)0)
#define RAE_CHAN_UNLOCK(c) ((void)0)
#endif

int64_t rae_ext_rae_chan_new(int64_t elem_size) {
  RaeChannel* c = (RaeChannel*)malloc(sizeof(RaeChannel));
  if (elem_size < 1) elem_size = 1;
  c->elem_size = elem_size;
  c->cap = RAE_CHAN_CAP;
  c->buf = (uint8_t*)calloc((size_t)c->cap, (size_t)elem_size);
  c->head = 0; c->count = 0; c->recv_count = 0;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_init(&c->mu, NULL);
#endif
  return (int64_t)(intptr_t)c;
}

/* The slot array, typed on the Rae side as Buffer(T). */
void* rae_ext_rae_chan_ring(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  return c ? (void*)c->buf : NULL;
}

/* Producer: lock and hand out the tail slot, or -1 (and no lock held) when
 * the ring is full. The caller stores into the slot, then commits. */
int64_t rae_ext_rae_chan_reserve(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return -1;
  RAE_CHAN_LOCK(c);
  if (c->count >= c->cap) { RAE_CHAN_UNLOCK(c); return -1; }
  return (c->head + c->count) % c->cap;
}

void rae_ext_rae_chan_commit(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return;
  c->count++;
  RAE_CHAN_UNLOCK(c);
}

/* Consumer: lock and hand out the head slot, or -1 (no lock held) when
 * empty. The caller copies the value out, then releases. */
int64_t rae_ext_rae_chan_take(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return -1;
  RAE_CHAN_LOCK(c);
  if (c->count <= 0) { RAE_CHAN_UNLOCK(c); return -1; }
  return c->head;
}

void rae_ext_rae_chan_release(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return;
  /* The slot's bytes were moved out; zero it so a stale pointer never sits
   * in the ring (a later diagnostic dump reads clean slots). */
  memset(c->buf + (size_t)c->head * (size_t)c->elem_size, 0, (size_t)c->elem_size);
  c->head = (c->head + 1) % c->cap;
  c->count--;
  c->recv_count++;
  RAE_CHAN_UNLOCK(c);
}

int64_t rae_ext_rae_chan_count(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return 0;
  RAE_CHAN_LOCK(c);
  int64_t n = c->count;
  RAE_CHAN_UNLOCK(c);
  return n;
}

int64_t rae_ext_rae_chan_received(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return 0;
  RAE_CHAN_LOCK(c);
  int64_t n = c->recv_count;
  RAE_CHAN_UNLOCK(c);
  return n;
}

void rae_ext_rae_chan_free(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_destroy(&c->mu);
#endif
  free(c->buf);
  free(c);
}


/* ----- Worker pool and parallel-for (`parallelLoop`) ----------------------
 *
 * A `parallelLoop var i: Int = start, i < end, ++i { body }` compiles to
 * rae_parallel_for(start, end, thunk, captures): the C backend outlines the
 * body into `thunk(captures, first, end)`, which runs iterations [first, end)
 * (docs/concurrency-model.md §2b; sema guarantees an iteration writes only its
 * own locals and the element of its own index, so the order iterations run in
 * cannot change any result).
 *
 * The pool: one worker per performance core but one, the caller being one
 * of them, started on the first parallelLoop, joined at exit. The core left
 * over is for the program's other threads (a renderer, the GPU driver, the
 * window server): with every performance core in the pool, those preempt a
 * worker in the middle of a chunk and the join waits out the time slice
 * (docs/physics-performance-plan.md §10h). `RAE_WORKERS=n` overrides the
 * count; `RAE_WORKERS=1` runs every parallelLoop sequentially on the caller.
 * Efficiency cores are left out on purpose: they would set the pace of every
 * join.
 *
 * One job at a time. The range is cut into fixed-size chunks (about four per
 * worker, so a slow worker is balanced by the others), claimed with one
 * atomic word that packs the job's generation with the next chunk index: a
 * worker still finishing the previous job cannot claim a chunk of the next
 * one, because its compare-exchange carries the old generation. After a job a
 * worker spins for a few tens of microseconds — a program that launches
 * parallel loops back to back pays no wake-up — and then sleeps on a
 * condition variable, so an idle program costs nothing.
 *
 * A parallelLoop inside a parallel body, or one started while another
 * thread's job is running, runs inline on its caller: same results, no
 * nested scheduling. */

/* RaeParallelBody is declared in rae_runtime.h. */

static __thread int g_pool_inside = 0;       /* running inside a parallel body */
static __thread int g_pool_worker_index = 0; /* 0 = the launching thread */

static inline void rae_cpu_relax(void) {
#if defined(__aarch64__) || defined(__arm64__)
  __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
  __asm__ __volatile__("pause" ::: "memory");
#endif
}

/* ----- RAE_PARALLEL_CHECK: are the `unsafe` writes really disjoint? ---------
 *
 * `unsafe { }` in a parallelLoop body lifts the compile-time write rule: the
 * programmer vouches that no two iterations write the same element (a graph
 * colouring's disjoint body writes, docs/parallel-stages-design.md §3). Every
 * List set/modAt in such a block first calls rae_parallel_check_write. With
 * checking on — RAE_PARALLEL_CHECK=1, or Parallel.checkWrites(enabled: true) —
 * the runtime records which iteration of the current top-level parallelLoop
 * wrote each (storage, index) and stops the program on a second writer,
 * naming both iterations (smaller first, so the message does not depend on
 * scheduling). Off, the call is one predictable branch. */
static int g_pcheck_enabled = -1;            /* -1: environment not read yet */
static _Atomic uint64_t g_pcheck_launch = 1; /* bumped per top-level launch while checking */

typedef struct { const void* storage; int64_t index; int64_t iteration; uint64_t launch; } RaePcheckEntry;
#define RAE_PCHECK_SIZE (1u << 18)
static RaePcheckEntry* g_pcheck_table = NULL;
static int g_pcheck_full_warned = 0;
static pthread_mutex_t g_pcheck_lock = PTHREAD_MUTEX_INITIALIZER;

static int rae_pcheck_on(void) {
  if (g_pcheck_enabled < 0) {
    const char* e = getenv("RAE_PARALLEL_CHECK");
    g_pcheck_enabled = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
  }
  return g_pcheck_enabled;
}

void rae_ext_Parallel_checkWrites(rae_Bool enabled) {
  g_pcheck_enabled = enabled ? 1 : 0;
}

void rae_parallel_check_write(const void* storage, int64_t index, int64_t iteration, const char* name) {
  if (!rae_pcheck_on() || !storage) return;
  uint64_t launch = atomic_load(&g_pcheck_launch);
  RAE_LOCK(&g_pcheck_lock);
  if (!g_pcheck_table) g_pcheck_table = (RaePcheckEntry*)calloc(RAE_PCHECK_SIZE, sizeof(RaePcheckEntry));
  if (!g_pcheck_table) { RAE_UNLOCK(&g_pcheck_lock); return; }
  uint64_t h = (uint64_t)(uintptr_t)storage * 0x9E3779B97F4A7C15ull ^ (uint64_t)index * 0xC2B2AE3D27D4EB4Full;
  for (uint32_t probe = 0; probe < 64; probe++) {
    RaePcheckEntry* e = &g_pcheck_table[(h + probe) & (RAE_PCHECK_SIZE - 1)];
    if (e->launch != launch) {           /* empty, or left from an earlier launch */
      e->storage = storage; e->index = index; e->iteration = iteration; e->launch = launch;
      RAE_UNLOCK(&g_pcheck_lock);
      return;
    }
    if (e->storage == storage && e->index == index) {
      if (e->iteration != iteration) {
        int64_t first = e->iteration < iteration ? e->iteration : iteration;
        int64_t second = e->iteration < iteration ? iteration : e->iteration;
        fflush(stdout);
        fprintf(stderr,
          "rae: parallelLoop: iterations %lld and %lld both wrote %s[%lld] inside `unsafe` — "
          "the writes are not disjoint (RAE_PARALLEL_CHECK)\n",
          (long long)first, (long long)second, name ? name : "a List", (long long)index);
        fflush(stderr);
        _Exit(70);
      }
      RAE_UNLOCK(&g_pcheck_lock);
      return;
    }
  }
  if (!g_pcheck_full_warned) {
    g_pcheck_full_warned = 1;
    fprintf(stderr, "rae: RAE_PARALLEL_CHECK: table full, some writes of this launch are not checked\n");
  }
  RAE_UNLOCK(&g_pcheck_lock);
}

/* A top-level launch starts a new set of writes to compare. */
static void rae_pcheck_new_launch(void) {
  if (g_pcheck_enabled == 1) atomic_fetch_add(&g_pcheck_launch, 1);
}

#if !defined(__wasm__) || defined(RAE_WASM_THREADS)

#include <sched.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <pthread/qos.h>
#endif

#define RAE_POOL_MAX 64
/* How long a worker spins after a job before it sleeps. A solver step runs a
 * few hundred parallelLoops a few microseconds apart, and a worker that fell
 * asleep between two of them costs a condition-variable wake-up (~5-15 us)
 * on the next one — while its range of chunks waits to be stolen. 250 us of
 * spinning covers the gaps of a step and costs an idle program nothing
 * measurable: it happens once, after the last job. Measured in time, because
 * a CPU relax instruction takes anywhere from one cycle to tens of them. */
#define RAE_POOL_SPIN_NS 250000

static struct {
  pthread_mutex_t lock;            /* guards sleeping workers and start-up */
  pthread_cond_t wake;
  pthread_t threads[RAE_POOL_MAX];
  int thread_count;                /* workers besides the launching thread */
  int started;
  _Atomic uint32_t generation;     /* bumped once per job */
  _Atomic int job_workers;         /* workers taking part (thread_count + 1) */
  _Atomic int64_t chunk_count;
  _Atomic int64_t chunks_done;
  _Atomic int sleeping;
  _Atomic int shutdown;
  /* The current job: written before `generation` is published, read only by
   * a worker that claimed one of its chunks. */
  RaeParallelBody body;
  void* captures;
  int64_t start, end, chunk;
} g_pool = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER };

static pthread_mutex_t g_pool_launch = PTHREAD_MUTEX_INITIALIZER;   /* one job at a time */

/* Each worker owns a contiguous range of the job's chunks and its own claim
 * word, (generation << 32) | next chunk index, on its own cache line (128
 * bytes covers Apple's). A worker drains its range first, then steals from
 * the others' — Box3D's solver does the same — so in the common case no two
 * workers ever touch one cache line, where a single shared claim word made
 * every claim a contended compare-exchange (4.7 us per launch at 8 workers,
 * benchmarks/parallel_stages). */
typedef struct {
  _Atomic uint64_t claim;
  char pad[128 - sizeof(uint64_t)];
} RaePoolSlot;
static RaePoolSlot g_pool_slots[RAE_POOL_MAX] __attribute__((aligned(128)));

static inline int64_t rae_pool_range_start(int worker, int64_t chunk_count, int workers) {
  return (int64_t)worker * chunk_count / workers;
}

static int rae_pool_default_workers(void) {
  const char* env = getenv("RAE_WORKERS");
  if (env && env[0]) {
    int n = atoi(env);
    return n < 1 ? 1 : (n > RAE_POOL_MAX ? RAE_POOL_MAX : n);
  }
  int n = 0;
#if defined(__APPLE__)
  int perf = 0; size_t size = sizeof perf;
  if (sysctlbyname("hw.perflevel0.physicalcpu", &perf, &size, NULL, 0) == 0 && perf > 0) n = perf;
#endif
  if (n <= 0) {
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    n = online > 0 ? (int)online : 1;
  }
  n = n > 1 ? n - 1 : 1;   /* leave one core to the program's other threads */
  return n > RAE_POOL_MAX ? RAE_POOL_MAX : n;
}

/* Claim and run chunks of job `generation` from one worker's range until it
 * is empty (or a new job started); returns how many it ran. */
static int64_t rae_pool_drain_slot(uint32_t generation, int slot, int64_t chunk_count, int workers) {
  int64_t ran = 0;
  int64_t end = rae_pool_range_start(slot + 1, chunk_count, workers);
  for (;;) {
    uint64_t claim = atomic_load_explicit(&g_pool_slots[slot].claim, memory_order_acquire);
    if ((uint32_t)(claim >> 32) != generation) return ran;
    int64_t index = (int64_t)(uint32_t)claim;
    if (index >= end) return ran;
    if (!atomic_compare_exchange_weak_explicit(&g_pool_slots[slot].claim, &claim, claim + 1,
                                               memory_order_acq_rel, memory_order_relaxed)) continue;
    /* This chunk is ours, so the job cannot finish (nor its fields change)
     * until it is counted done. */
    int64_t first = g_pool.start + index * g_pool.chunk;
    int64_t last = first + g_pool.chunk;
    if (last > g_pool.end) last = g_pool.end;
    g_pool.body(g_pool.captures, first, last);
    ran++;
  }
}

/* Run chunks of job `generation`: this worker's own range, then steal from
 * the others in order. Counts what it ran once, at the end. */
static void rae_pool_run_chunks(uint32_t generation, int self) {
  int64_t chunk_count = atomic_load_explicit(&g_pool.chunk_count, memory_order_relaxed);
  int workers = atomic_load_explicit(&g_pool.job_workers, memory_order_relaxed);
  if (workers < 1 || self >= workers) return;
  int64_t ran = 0;
  for (int k = 0; k < workers; k++) ran += rae_pool_drain_slot(generation, (self + k) % workers, chunk_count, workers);
  if (ran) atomic_fetch_add_explicit(&g_pool.chunks_done, ran, memory_order_release);
}

static int64_t rae_pool_now_ns(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static void* rae_pool_worker(void* arg) {
  g_pool_worker_index = (int)(intptr_t)arg;
  g_pool_inside = 1;
  uint32_t seen = atomic_load(&g_pool.generation);
  for (;;) {
    uint32_t generation;
    int spins = 0;
    int64_t spin_until = rae_pool_now_ns() + RAE_POOL_SPIN_NS;
    while ((generation = atomic_load(&g_pool.generation)) == seen) {
      if (atomic_load(&g_pool.shutdown)) return NULL;
      /* Read the clock only every 256 relaxes: it costs more than one. */
      if ((++spins & 255) != 0 || rae_pool_now_ns() < spin_until) { rae_cpu_relax(); continue; }
      pthread_mutex_lock(&g_pool.lock);
      atomic_fetch_add(&g_pool.sleeping, 1);
      while (atomic_load(&g_pool.generation) == seen && !atomic_load(&g_pool.shutdown))
        pthread_cond_wait(&g_pool.wake, &g_pool.lock);
      atomic_fetch_sub(&g_pool.sleeping, 1);
      pthread_mutex_unlock(&g_pool.lock);
      spins = 0;
      spin_until = rae_pool_now_ns() + RAE_POOL_SPIN_NS;
    }
    seen = generation;
    rae_pool_run_chunks(generation, g_pool_worker_index);
  }
}

static void rae_pool_shutdown(void) {
  if (!g_pool.started || g_pool_worker_index != 0 || g_pool_inside) return;
  atomic_store(&g_pool.shutdown, 1);
  pthread_mutex_lock(&g_pool.lock);
  pthread_cond_broadcast(&g_pool.wake);
  pthread_mutex_unlock(&g_pool.lock);
  for (int i = 0; i < g_pool.thread_count; i++) pthread_join(g_pool.threads[i], NULL);
  g_pool.thread_count = 0;
}

static _Atomic int g_pool_ready_count = 0;   /* workers incl. the caller, once started */

/* Start the pool once; returns the number of workers including the caller.
 * Every launch asks, so after the first the answer is one atomic load. */
static int rae_pool_start(void) {
  int ready = atomic_load_explicit(&g_pool_ready_count, memory_order_acquire);
  if (ready) return ready;
  pthread_mutex_lock(&g_pool.lock);
  if (!g_pool.started) {
    g_pool.started = 1;
    int workers = rae_pool_default_workers();
    /* The workers run at the launching thread's QoS class. macOS starts a
     * new thread at DEFAULT, below an app's main thread (USER_INTERACTIVE),
     * while that thread waits on them at every join: under CPU load from
     * other processes a preempted worker then holds a chunk for a whole time
     * slice and the join waits it out. The physics step spiked to 8-30 ms
     * that way (docs/physics-performance-plan.md §10h: 184 steps over 8 ms
     * in 1 200 frames at DEFAULT, 5 at the launcher's class). */
    pthread_attr_t attributes;
    pthread_attr_t* worker_attributes = NULL;
    if (pthread_attr_init(&attributes) == 0) {
      worker_attributes = &attributes;
#if defined(__APPLE__)
      qos_class_t launcher_qos = qos_class_self();
      if (launcher_qos != QOS_CLASS_UNSPECIFIED) pthread_attr_set_qos_class_np(&attributes, launcher_qos, 0);
#endif
    }
    for (int i = 0; i < workers - 1; i++) {
      if (pthread_create(&g_pool.threads[g_pool.thread_count], worker_attributes, rae_pool_worker,
                         (void*)(intptr_t)(i + 1)) != 0) break;
      g_pool.thread_count++;
    }
    if (worker_attributes) pthread_attr_destroy(worker_attributes);
    atexit(rae_pool_shutdown);
  }
  int count = g_pool.thread_count + 1;
  atomic_store_explicit(&g_pool_ready_count, count, memory_order_release);
  pthread_mutex_unlock(&g_pool.lock);
  return count;
}

int64_t rae_ext_Parallel_workerCount(void) {
  return (int64_t)rae_pool_start();
}

void rae_parallel_for(int64_t start, int64_t end, RaeParallelBody body, void* captures) {
  if (end <= start) return;
  if (g_pool_inside) { body(captures, start, end); return; }
  rae_pcheck_on();
  rae_pcheck_new_launch();
  int workers = rae_pool_start();
  int64_t total = end - start;
  if (workers <= 1 || total == 1 || pthread_mutex_trylock(&g_pool_launch) != 0) {
    int saved = g_pool_inside;
    g_pool_inside = 1;
    body(captures, start, end);
    g_pool_inside = saved;
    return;
  }
  int64_t target_chunks = (int64_t)workers * 4;
  int64_t chunk = (total + target_chunks - 1) / target_chunks;
  if (chunk < 1) chunk = 1;
  int64_t chunk_count = (total + chunk - 1) / chunk;
  if (chunk_count <= 1) {
    /* One chunk: run it here; publishing would only add the join. */
    pthread_mutex_unlock(&g_pool_launch);
    g_pool_inside = 1;
    body(captures, start, end);
    g_pool_inside = 0;
    return;
  }
  uint32_t generation = atomic_load(&g_pool.generation) + 1;
  /* Reset the claim words first: a worker still holding the old generation
   * now fails every compare-exchange, so it cannot see the new fields. */
  for (int w = 0; w < workers; w++)
    atomic_store_explicit(&g_pool_slots[w].claim,
                          ((uint64_t)generation << 32) | (uint64_t)rae_pool_range_start(w, chunk_count, workers),
                          memory_order_relaxed);
  atomic_store_explicit(&g_pool.job_workers, workers, memory_order_relaxed);
  g_pool.body = body;
  g_pool.captures = captures;
  g_pool.start = start;
  g_pool.end = end;
  g_pool.chunk = chunk;
  atomic_store(&g_pool.chunk_count, chunk_count);
  atomic_store(&g_pool.chunks_done, 0);
  atomic_store(&g_pool.generation, generation);
  if (atomic_load(&g_pool.sleeping) > 0) {
    pthread_mutex_lock(&g_pool.lock);
    pthread_cond_broadcast(&g_pool.wake);
    pthread_mutex_unlock(&g_pool.lock);
  }
  g_pool_inside = 1;
  rae_pool_run_chunks(generation, 0);
  g_pool_inside = 0;
  int spins = 0;
  while (atomic_load_explicit(&g_pool.chunks_done, memory_order_acquire) < chunk_count) {
    if (++spins < (1 << 16)) rae_cpu_relax();
    else sched_yield();
  }
  pthread_mutex_unlock(&g_pool_launch);
}

#else  /* a WASM build without threads: every parallelLoop runs on its caller */

int64_t rae_ext_Parallel_workerCount(void) { return 1; }

void rae_parallel_for(int64_t start, int64_t end, RaeParallelBody body, void* captures) {
  rae_pcheck_on();
  rae_pcheck_new_launch();
  if (end > start) body(captures, start, end);
}

#endif
