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
 * the worker pool's threads by their worker index, and 64..71 are claimed at
 * run time by named threads: a `spawn thread` (named by its function, the C
 * backend passes it to rae_task_start_thread) and a foreign thread
 * (rae_thread_attach_named, or "attached thread" for rae_thread_attach). A
 * named thread claims its slot at its first event while a table is installed,
 * once per table; the Rae collector reads the slot's name
 * (rae_perf_thread_name) and names the lane. A thread with no slot, or past
 * the eighth, records nothing. */

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
  RAE_PERF_SIGNAL_POST = 10,  /* a Signal post (a wake) */
  RAE_PERF_COMMAND_BUFFER = 11, /* a Metal command buffer's GPU time (runtime_gpu_metal_timing.c) */
  RAE_PERF_RULE_BREAK = 12      /* RAE_REALTIME_CHECK: a real-time thread allocated, freed, locked or
                                   logged; extra = its slot in runtime_realtime.c (-1: past them) */
};

/* The kinds, in lib/perf/Perf's PerfKind order */
enum {
  RAE_PERF_ZONE_BEGIN = 0,
  RAE_PERF_ZONE_END = 1,
  RAE_PERF_MARK = 3,
  RAE_PERF_WAIT_BEGIN = 4,
  RAE_PERF_WAIT_END = 5,
  RAE_PERF_GPU_SPAN = 6,
  RAE_PERF_FLOW = 7
};

#define RAE_PERF_SLOTS 64
#define RAE_PERF_THREAD_SLOTS 8

static _Atomic(void*) g_rae_perf_rings = NULL;   /* the hub's Buffer(PerfRing), one per slot */
static _Atomic(void*) g_rae_perf_names = NULL;   /* its Buffer(Int): event code -> name id */
static _Atomic(void*) g_rae_perf_name_box = NULL; /* the hub's name list, boxed (main thread only) */
static _Atomic int64_t g_rae_perf_table_generation = 0; /* bumped at every install and uninstall */
static _Thread_local int64_t g_rae_perf_slot = -1;

/* The named threads' slots: each table (a generation) hands them out anew */
static _Atomic(const char*) g_rae_perf_thread_names[RAE_PERF_THREAD_SLOTS];
static _Atomic int64_t g_rae_perf_threads_claimed = 0;
static _Atomic int64_t g_rae_perf_generation = 0;
static _Thread_local const char* g_rae_perf_thread_name = NULL;
static _Thread_local int64_t g_rae_perf_thread_generation = -1;

/* A named thread's slot in the installed table, claimed on its first event */
static int64_t rae_perf_claim_slot(void) {
  int64_t generation = atomic_load_explicit(&g_rae_perf_generation, memory_order_acquire);
  if (g_rae_perf_thread_generation == generation) return g_rae_perf_slot;
  g_rae_perf_thread_generation = generation;
  g_rae_perf_slot = -1;
  int64_t index = atomic_fetch_add_explicit(&g_rae_perf_threads_claimed, 1, memory_order_relaxed);
  if (index >= RAE_PERF_THREAD_SLOTS) return -1;
  atomic_store_explicit(&g_rae_perf_thread_names[index], g_rae_perf_thread_name, memory_order_release);
  g_rae_perf_slot = RAE_PERF_SLOTS + index;
  return g_rae_perf_slot;
}

/* Name the calling thread for the recorder (a string that lives as long as the program) */
static void rae_perf_name_thread(const char* name) {
  g_rae_perf_thread_name = name;
  g_rae_perf_thread_generation = -1;
}

/* lib/perf/Perf's perfRuntimeWrite (`cExport`); a program that does not use
 * the recorder links this empty one */
__attribute__((weak)) void rae_perf_runtime_write(void* rings, void* names, int64_t slot, int64_t kind,
                                                  int64_t code, int64_t value, int64_t extra) {
  (void)rings; (void)names; (void)slot; (void)kind; (void)code; (void)value; (void)extra;
}

/* lib/perf/Perf's perfRuntimeWriteAt (`cExport`): an event at a time of its own */
__attribute__((weak)) void rae_perf_runtime_write_at(void* rings, void* names, int64_t slot, int64_t kind,
                                                     int64_t code, int64_t time, int64_t value, int64_t extra) {
  (void)rings; (void)names; (void)slot; (void)kind; (void)code; (void)time; (void)value; (void)extra;
}

static inline void rae_perf_event(int64_t kind, int64_t code, int64_t value, int64_t extra) {
  void* rings = atomic_load_explicit(&g_rae_perf_rings, memory_order_acquire);
  if (__builtin_expect(rings == NULL, 1)) return;
  int64_t slot = g_rae_perf_thread_name ? rae_perf_claim_slot() : g_rae_perf_slot;
  if (slot < 0) return;
  rae_perf_runtime_write(rings, atomic_load_explicit(&g_rae_perf_names, memory_order_acquire), slot, kind, code,
                         value, extra);
}

#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
/* One lane shared by threads that come and go (Metal's completion handlers run on any of GCD's
 * threads, so a slot per thread would use them all up): its writers take turns under a lock, and
 * uninstalling a table takes it too, so no writer holds a table's rings after it is gone */
static pthread_mutex_t g_rae_perf_shared_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_rae_perf_shared_slot = -1;        /* guarded by the lock */
static int64_t g_rae_perf_shared_generation = -1;  /* guarded by the lock */

/* A span with times of its own (`begin_ns` .. `end_ns` on nowNs's clock) on the shared lane named
 * `name` (a string that lives as long as the program) */
__attribute__((unused)) static void rae_perf_shared_span(const char* name, int64_t code, int64_t begin_ns,
                                                         int64_t end_ns) {
  if (atomic_load_explicit(&g_rae_perf_rings, memory_order_acquire) == NULL) return;
  pthread_mutex_lock(&g_rae_perf_shared_lock);
  void* rings = atomic_load_explicit(&g_rae_perf_rings, memory_order_acquire);
  int64_t generation = atomic_load_explicit(&g_rae_perf_generation, memory_order_acquire);
  if (rings && g_rae_perf_shared_generation != generation) {
    g_rae_perf_shared_generation = generation;
    g_rae_perf_shared_slot = -1;
    int64_t index = atomic_fetch_add_explicit(&g_rae_perf_threads_claimed, 1, memory_order_relaxed);
    if (index < RAE_PERF_THREAD_SLOTS) {
      atomic_store_explicit(&g_rae_perf_thread_names[index], name, memory_order_release);
      g_rae_perf_shared_slot = RAE_PERF_SLOTS + index;
    }
  }
  if (rings && g_rae_perf_shared_slot >= 0)
    rae_perf_runtime_write_at(rings, atomic_load_explicit(&g_rae_perf_names, memory_order_acquire),
                              g_rae_perf_shared_slot, RAE_PERF_GPU_SPAN, code, begin_ns, end_ns, 0);
  pthread_mutex_unlock(&g_rae_perf_shared_lock);
}
#endif

/* A pool worker's slot is its worker index */
static inline void rae_perf_set_worker_slot(int64_t worker) {
  g_rae_perf_slot = worker > 0 && worker < RAE_PERF_SLOTS ? worker : -1;
}

/* Install a hub's table (null: none); the calling thread becomes slot 0. The
 * hub frees its rings only after installing null, on the main thread, between
 * parallel jobs. */
void rae_perf_install(void* rings, void* names, void* name_box) {
  g_rae_perf_slot = 0;
  g_rae_perf_thread_name = NULL;
  for (int i = 0; i < RAE_PERF_THREAD_SLOTS; i++)
    atomic_store_explicit(&g_rae_perf_thread_names[i], NULL, memory_order_relaxed);
  atomic_store_explicit(&g_rae_perf_threads_claimed, 0, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_rae_perf_generation, 1, memory_order_release);
  atomic_store_explicit(&g_rae_perf_names, names, memory_order_release);
  atomic_store_explicit(&g_rae_perf_name_box, name_box, memory_order_release);
  atomic_fetch_add_explicit(&g_rae_perf_table_generation, 1, memory_order_acq_rel);
  atomic_store_explicit(&g_rae_perf_rings, rings, memory_order_release);
}

/* Remove `rings` if they are the installed table (a later hub's stays) */
void rae_perf_uninstall(void* rings) {
  void* expected = rings;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  /* under the shared lane's lock: its writer loads the rings under it, so none holds them after */
  pthread_mutex_lock(&g_rae_perf_shared_lock);
#endif
  if (atomic_compare_exchange_strong(&g_rae_perf_rings, &expected, NULL)) {
    atomic_store_explicit(&g_rae_perf_name_box, NULL, memory_order_release);
    atomic_fetch_add_explicit(&g_rae_perf_table_generation, 1, memory_order_acq_rel);
  }
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_unlock(&g_rae_perf_shared_lock);
#endif
}

/* The installed table's generation, 0 when none is installed: code that caches name ids (lib/ecs's
 * schedule) knows from it when they are stale */
int64_t rae_perf_generation(void) {
  if (atomic_load_explicit(&g_rae_perf_rings, memory_order_acquire) == NULL) return 0;
  return atomic_load_explicit(&g_rae_perf_table_generation, memory_order_acquire);
}

/* The installed hub's boxed name list, for the thread that installed it only (null elsewhere, or
 * with no table): registering a name allocates, so only that thread may */
void* rae_perf_name_box(void) {
  if (g_rae_perf_slot != 0 || g_rae_perf_thread_name) return NULL;
  return atomic_load_explicit(&g_rae_perf_name_box, memory_order_acquire);
}

rae_Bool rae_perf_name_box_ready(void) {
  return rae_perf_name_box() != NULL;
}

/* An event named by a name id of the installed hub (rae_perf_name_box), on the calling thread's
 * lane; the code passed on is -1 - id */
void rae_perf_named_event(int64_t kind, int64_t name_id, int64_t value, int64_t extra) {
  rae_perf_event(kind, -1 - name_id, value, extra);
}

int64_t rae_perf_slot_count(void) {
  return RAE_PERF_SLOTS + RAE_PERF_THREAD_SLOTS;
}

/* The first slot a named thread claims, and how many there are */
int64_t rae_perf_thread_slot_first(void) {
  return RAE_PERF_SLOTS;
}

int64_t rae_perf_thread_slot_count(void) {
  return RAE_PERF_THREAD_SLOTS;
}

/* The name of the thread that claimed named slot `index` ("" while none has) */
rae_String rae_perf_thread_name(int64_t index) {
  const char* name = index >= 0 && index < RAE_PERF_THREAD_SLOTS
                         ? atomic_load_explicit(&g_rae_perf_thread_names[index], memory_order_acquire)
                         : NULL;
  return rae_ext_rae_str_from_cstr(name ? name : "");
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
