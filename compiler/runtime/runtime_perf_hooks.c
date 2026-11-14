/* runtime_perf_hooks.c — where the runtime's own pool, tasks, channels, locks
 * and signals tell the event recorder (lib/perf/Perf, docs/perf-tools-design.md
 * §4.3) what they did. The recorder is Rae: the rings, the names and every
 * decision about an event live there. This file only knows which thread slot
 * the calling thread is and passes an event code to one Rae function, so each
 * hook is one call and holds no policy (the runtime ABI exception: the pool's
 * and the task entry points' hooks, docs/perf-tools-design.md §11).
 *
 * Nothing is recorded until a program installs a hub's runtime table
 * (perfInstallRuntime). Until then, and for a thread with no slot, a hook is
 * one relaxed atomic load.
 *
 * Slots: 0 is the thread that installed the table (the main thread), 1..63 are
 * the worker pool's threads by their worker index. Other threads (a `spawn
 * thread`, an attached foreign thread) have none yet and record nothing. */

/* The event codes; lib/perf/Perf's perfRuntimeCode* constants are the same */
enum {
  RAE_PERF_PARALLEL_LOOP = 0, /* a parallelLoop on its caller's lane */
  RAE_PERF_CHUNK = 1,         /* a chunk of it on the lane of the worker that ran it */
  RAE_PERF_TASK_GET = 2,      /* Task.get waiting for its task */
  RAE_PERF_TASK_JOIN = 3,     /* a Task joined as its scope ends */
  RAE_PERF_TASK_SCOPE = 4,    /* a taskScope waiting for its tasks */
  RAE_PERF_TASK_RUN = 5,      /* a task running on the pool */
  RAE_PERF_SPAWN = 6,         /* a flow from a spawn to the task's run */
  RAE_PERF_CHANNEL_LOCK = 7,  /* a Channel send or receive waiting for the channel */
  RAE_PERF_LOCK = 8,          /* a contended runtime lock (RAE_LOCK) */
  RAE_PERF_SIGNAL_WAIT = 9,   /* a Signal wait */
  RAE_PERF_SIGNAL_POST = 10   /* a Signal post (a wake) */
};

/* The kinds, in lib/perf/Perf's PerfKind order */
enum {
  RAE_PERF_ZONE_BEGIN = 0,
  RAE_PERF_ZONE_END = 1,
  RAE_PERF_MARK = 3,
  RAE_PERF_WAIT_BEGIN = 4,
  RAE_PERF_WAIT_END = 5,
  RAE_PERF_FLOW = 7
};

#define RAE_PERF_SLOTS 64

static _Atomic(void*) g_rae_perf_rings = NULL;   /* the hub's Buffer(PerfRing), one per slot */
static _Atomic(void*) g_rae_perf_names = NULL;   /* its Buffer(Int): event code -> name id */
static _Thread_local int64_t g_rae_perf_slot = -1;

/* lib/perf/Perf's perfRuntimeWrite (`cExport`); a program that does not use
 * the recorder links this empty one */
__attribute__((weak)) void rae_perf_runtime_write(void* rings, void* names, int64_t slot, int64_t kind,
                                                  int64_t code, int64_t value, int64_t extra) {
  (void)rings; (void)names; (void)slot; (void)kind; (void)code; (void)value; (void)extra;
}

static inline void rae_perf_event(int64_t kind, int64_t code, int64_t value, int64_t extra) {
  void* rings = atomic_load_explicit(&g_rae_perf_rings, memory_order_acquire);
  if (__builtin_expect(rings == NULL, 1) || g_rae_perf_slot < 0) return;
  rae_perf_runtime_write(rings, atomic_load_explicit(&g_rae_perf_names, memory_order_acquire), g_rae_perf_slot,
                         kind, code, value, extra);
}

/* A pool worker's slot is its worker index */
static inline void rae_perf_set_worker_slot(int64_t worker) {
  g_rae_perf_slot = worker > 0 && worker < RAE_PERF_SLOTS ? worker : -1;
}

/* Install a hub's table (null: none); the calling thread becomes slot 0. The
 * hub frees its rings only after installing null, on the main thread, between
 * parallel jobs. */
void rae_perf_install(void* rings, void* names) {
  g_rae_perf_slot = 0;
  atomic_store_explicit(&g_rae_perf_names, names, memory_order_release);
  atomic_store_explicit(&g_rae_perf_rings, rings, memory_order_release);
}

/* Remove `rings` if they are the installed table (a later hub's stays) */
void rae_perf_uninstall(void* rings) {
  void* expected = rings;
  atomic_compare_exchange_strong(&g_rae_perf_rings, &expected, NULL);
}

int64_t rae_perf_slot_count(void) {
  return RAE_PERF_SLOTS;
}

#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
/* Take `mutex`; when another thread holds it, the wait is a span */
static inline void rae_perf_lock(pthread_mutex_t* mutex, int64_t code) {
  if (pthread_mutex_trylock(mutex) == 0) return;
  rae_perf_event(RAE_PERF_WAIT_BEGIN, code, 0, 0);
  pthread_mutex_lock(mutex);
  rae_perf_event(RAE_PERF_WAIT_END, code, 0, 0);
}
#endif
