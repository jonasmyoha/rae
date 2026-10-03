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
  t->done = 0;
  t->joined = 0;
  return t;
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
 * The pool: one thread per performance core minus one (the caller is the
 * last worker), started on the first parallelLoop, joined at exit.
 * `RAE_WORKERS=n` overrides the count; `RAE_WORKERS=1` runs every
 * parallelLoop sequentially on the caller. Efficiency cores are left out on
 * purpose: they would set the pace of every join.
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

#if !defined(__wasm__) || defined(RAE_WASM_THREADS)

#include <sched.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#define RAE_POOL_MAX 64
#define RAE_POOL_SPIN 20000   /* relax iterations before sleeping (~tens of microseconds) */

static struct {
  pthread_mutex_t lock;            /* guards sleeping workers and start-up */
  pthread_cond_t wake;
  pthread_t threads[RAE_POOL_MAX];
  int thread_count;                /* workers besides the launching thread */
  int started;
  _Atomic uint32_t generation;     /* bumped once per job */
  _Atomic uint64_t claim;          /* (generation << 32) | next chunk index */
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
  return n > RAE_POOL_MAX ? RAE_POOL_MAX : n;
}

/* Run chunks of job `generation` until none is left (or a new job started). */
static void rae_pool_run_chunks(uint32_t generation) {
  for (;;) {
    uint64_t claim = atomic_load_explicit(&g_pool.claim, memory_order_acquire);
    if ((uint32_t)(claim >> 32) != generation) return;
    int64_t index = (int64_t)(uint32_t)claim;
    if (index >= atomic_load_explicit(&g_pool.chunk_count, memory_order_relaxed)) return;
    if (!atomic_compare_exchange_weak_explicit(&g_pool.claim, &claim, claim + 1,
                                               memory_order_acq_rel, memory_order_relaxed)) continue;
    /* This chunk is ours, so the job cannot finish (nor its fields change)
     * until it is counted done below. */
    int64_t first = g_pool.start + index * g_pool.chunk;
    int64_t last = first + g_pool.chunk;
    if (last > g_pool.end) last = g_pool.end;
    g_pool.body(g_pool.captures, first, last);
    atomic_fetch_add_explicit(&g_pool.chunks_done, 1, memory_order_release);
  }
}

static void* rae_pool_worker(void* arg) {
  g_pool_worker_index = (int)(intptr_t)arg;
  g_pool_inside = 1;
  uint32_t seen = atomic_load(&g_pool.generation);
  for (;;) {
    uint32_t generation;
    int spins = 0;
    while ((generation = atomic_load(&g_pool.generation)) == seen) {
      if (atomic_load(&g_pool.shutdown)) return NULL;
      if (++spins < RAE_POOL_SPIN) { rae_cpu_relax(); continue; }
      pthread_mutex_lock(&g_pool.lock);
      atomic_fetch_add(&g_pool.sleeping, 1);
      while (atomic_load(&g_pool.generation) == seen && !atomic_load(&g_pool.shutdown))
        pthread_cond_wait(&g_pool.wake, &g_pool.lock);
      atomic_fetch_sub(&g_pool.sleeping, 1);
      pthread_mutex_unlock(&g_pool.lock);
      spins = 0;
    }
    seen = generation;
    rae_pool_run_chunks(generation);
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

/* Start the pool once; returns the number of workers including the caller. */
static int rae_pool_start(void) {
  pthread_mutex_lock(&g_pool.lock);
  if (!g_pool.started) {
    g_pool.started = 1;
    int workers = rae_pool_default_workers();
    for (int i = 0; i < workers - 1; i++) {
      if (pthread_create(&g_pool.threads[g_pool.thread_count], NULL, rae_pool_worker,
                         (void*)(intptr_t)(i + 1)) != 0) break;
      g_pool.thread_count++;
    }
    atexit(rae_pool_shutdown);
  }
  int count = g_pool.thread_count + 1;
  pthread_mutex_unlock(&g_pool.lock);
  return count;
}

int64_t rae_ext_Parallel_workerCount(void) {
  return (int64_t)rae_pool_start();
}

void rae_parallel_for(int64_t start, int64_t end, RaeParallelBody body, void* captures) {
  if (end <= start) return;
  if (g_pool_inside) { body(captures, start, end); return; }
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
  uint32_t generation = atomic_load(&g_pool.generation) + 1;
  /* Reset the claim word first: a worker still holding the old generation
   * now fails every compare-exchange, so it cannot see the new fields. */
  atomic_store(&g_pool.claim, (uint64_t)generation << 32);
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
  rae_pool_run_chunks(generation);
  g_pool_inside = 0;
  int spins = 0;
  while (atomic_load_explicit(&g_pool.chunks_done, memory_order_acquire) < chunk_count) {
    if (++spins < RAE_POOL_SPIN) rae_cpu_relax();
    else sched_yield();
  }
  pthread_mutex_unlock(&g_pool_launch);
}

#else  /* a WASM build without threads: every parallelLoop runs on its caller */

int64_t rae_ext_Parallel_workerCount(void) { return 1; }

void rae_parallel_for(int64_t start, int64_t end, RaeParallelBody body, void* captures) {
  if (end > start) body(captures, start, end);
}

#endif
