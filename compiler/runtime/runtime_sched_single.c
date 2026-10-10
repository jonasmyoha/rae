/* The scheduler's kernel in a build without threads (the browser, wasi;
 * included by runtime_threads.c). There is no pool: Scheduler.rae runs every
 * task on the one thread. A resumable task still suspends (on a timer, or on
 * another task) and is queued again when it can go on: in the browser the
 * frames of `mainLoop` run those (docs/lightweight-spawn-design.md §16), and
 * a wait on a task runs them until it is done. */

int64_t rae_ext_Parallel_workerCount(void) { return 1; }

static struct {
  void (*task_wait)(int64_t task);
  void (*wake)(int64_t task);
  void (*group_wait)(int64_t group);
} g_sched_hooks;

void rae_sched_install(void (*worker_loop)(int64_t index), void (*task_wait)(int64_t task),
                       int64_t (*default_workers)(void), int64_t (*chunk_size)(int64_t total, int64_t workers),
                       void (*wake)(int64_t task), void (*group_wait)(int64_t group)) {
  (void)worker_loop; (void)default_workers; (void)chunk_size;
  g_sched_hooks.task_wait = task_wait;
  g_sched_hooks.wake = wake;
  g_sched_hooks.group_wait = group_wait;
}

/* The resumable task the thread is running (rae_sched_wait_task and
 * rae_sched_sleep_until suspend it) */
static RaeTask* g_sched_current_task = NULL;

void rae_task_complete(RaeTask* t) {
  intptr_t waiter = atomic_exchange(&t->waiter, (intptr_t)-1);
  atomic_store_explicit(&t->done, 1, memory_order_release);
  if (waiter > 0 && g_sched_hooks.wake) g_sched_hooks.wake((int64_t)waiter);
}

int rae_sched_wait_task(RaeTask* t) {
  RaeTask* current = g_sched_current_task;
  if (!t || !current || current == t || rae_task_is_done(t)) return 0;
  intptr_t expected = 0;
  return atomic_compare_exchange_strong(&t->waiter, &expected, (intptr_t)current) ? 1 : 0;
}

int rae_sched_sleep_until(int64_t deadline_ns) {
  RaeTask* current = g_sched_current_task;
  if (!current || rae_sched_now_ns() >= deadline_ns) return 0;
  rae_sched_timer_push(deadline_ns, current);
  return 1;
}

void rae_group_done(RaeTaskGroup* g) { atomic_fetch_sub(&g->pending, 1); }

int64_t rae_ext_Scheduler_groupPending(int64_t group) {
  RaeTaskGroup* g = (RaeTaskGroup*)(intptr_t)group;
  return g ? atomic_load(&g->pending) : 0;
}

/* Nothing to block on: the one thread runs the tasks */
void rae_ext_Scheduler_waitGroup(int64_t group, int64_t timeout_ns) { (void)group; (void)timeout_ns; }

void rae_group_wait(RaeTaskGroup* g) {
  if (atomic_load(&g->pending) == 0) return;
  if (g_sched_hooks.group_wait) g_sched_hooks.group_wait((int64_t)(intptr_t)g);
}

void* rae_frame_slot(void** slot, size_t size) {
  if (!*slot) *slot = calloc(1, size < 16 ? 16 : size);
  return *slot;
}

void rae_frame_slots_free(void** slots, int count) { for (int i = 0; i < count; i++) free(slots[i]); }

/* The one queue (every queue number names it): first in, first out, and
 * newest-first for queuePopNewest */
static RaeTask** g_sched_ring = NULL;
static int64_t g_sched_ring_head = 0;
static int64_t g_sched_ring_count = 0;
static int64_t g_sched_ring_cap = 0;

void rae_ext_Scheduler_queuePush(int64_t queue, int64_t task) {
  (void)queue;
  if (g_sched_ring_count == g_sched_ring_cap) {
    int64_t cap = g_sched_ring_cap ? g_sched_ring_cap * 2 : 64;
    RaeTask** ring = (RaeTask**)malloc((size_t)cap * sizeof(RaeTask*));
    for (int64_t i = 0; i < g_sched_ring_count; i++) {
      ring[i] = g_sched_ring[(g_sched_ring_head + i) % g_sched_ring_cap];
    }
    free(g_sched_ring);
    g_sched_ring = ring;
    g_sched_ring_head = 0;
    g_sched_ring_cap = cap;
  }
  g_sched_ring[(g_sched_ring_head + g_sched_ring_count) % g_sched_ring_cap] = (RaeTask*)(intptr_t)task;
  g_sched_ring_count++;
}

int64_t rae_ext_Scheduler_queuePopOldest(int64_t queue) {
  (void)queue;
  if (g_sched_ring_count == 0) return 0;
  RaeTask* task = g_sched_ring[g_sched_ring_head];
  g_sched_ring_head = (g_sched_ring_head + 1) % g_sched_ring_cap;
  g_sched_ring_count--;
  return (int64_t)(intptr_t)task;
}

int64_t rae_ext_Scheduler_queuePopNewest(int64_t queue) {
  (void)queue;
  if (g_sched_ring_count == 0) return 0;
  g_sched_ring_count--;
  return (int64_t)(intptr_t)g_sched_ring[(g_sched_ring_head + g_sched_ring_count) % g_sched_ring_cap];
}

int64_t rae_ext_Scheduler_queueTryPopOldest(int64_t queue) { return rae_ext_Scheduler_queuePopOldest(queue); }

int64_t rae_ext_Scheduler_queueCount(int64_t queue) {
  (void)queue;
  return g_sched_ring_count;
}

/* Take `task` out of the newest `window` entries: true when it was there */
rae_Bool rae_ext_Scheduler_queueTake(int64_t queue, int64_t task, int64_t window) {
  (void)queue;
  for (int64_t k = 0; k < window && k < g_sched_ring_count; k++) {
    int64_t at = g_sched_ring_count - 1 - k;
    if ((int64_t)(intptr_t)g_sched_ring[(g_sched_ring_head + at) % g_sched_ring_cap] != task) continue;
    for (int64_t i = at; i < g_sched_ring_count - 1; i++) {
      g_sched_ring[(g_sched_ring_head + i) % g_sched_ring_cap] = g_sched_ring[(g_sched_ring_head + i + 1) % g_sched_ring_cap];
    }
    g_sched_ring_count--;
    return 1;
  }
  return 0;
}

int64_t rae_ext_Scheduler_currentWorker(void) { return -1; }
int64_t rae_ext_Scheduler_helpEnter(void) { return 1; }
void rae_ext_Scheduler_helpLeave(void) {}
int64_t rae_ext_Scheduler_parkEpoch(void) { return 0; }

/* The one thread does not sleep: a wait for a task that is on a timer looks
 * again at once (the browser's main thread may not block, and a wasi sleep
 * needs an import a host may not give) */
void rae_ext_Scheduler_park(int64_t epoch, int64_t timeout_ns) { (void)epoch; (void)timeout_ns; }

void rae_ext_Scheduler_unpark(void) {}

/* A step of a resumable task (until it finishes or suspends), or a plain
 * task to its end */
void rae_ext_Scheduler_runTask(int64_t task) {
  RaeTask* t = (RaeTask*)(intptr_t)task;
  if (!t) return;
  RaeTask* saved_task = g_sched_current_task;
  int pool_mark = rae_string_pool_mark();
  if (t->step) {
    g_sched_current_task = t;
    t->step(t->args);
  } else {
    t->thunk(t->args);
  }
  rae_string_pool_release(pool_mark);
  g_sched_current_task = saved_task;
}

rae_Bool rae_ext_Scheduler_taskIsDone(int64_t task) { return rae_task_is_done((RaeTask*)(intptr_t)task) ? 1 : 0; }
void rae_ext_Scheduler_waitDone(int64_t task, int64_t timeout_ns) { (void)task; (void)timeout_ns; }
rae_Bool rae_ext_Scheduler_helpParallel(void) { return 0; }
rae_Bool rae_ext_Scheduler_shuttingDown(void) { return 0; }
int64_t rae_ext_Scheduler_start(void) { return 0; }
int64_t rae_ext_Scheduler_performanceCores(void) { return 1; }
int64_t rae_ext_Scheduler_maxWorkers(void) { return 1; }

/* A task that has not finished is run, with the ones it waits on, by
 * Scheduler.rae's task wait */
static void rae_sched_wait(RaeTask* t) {
  if (rae_task_is_done(t)) return;
  if (g_sched_hooks.task_wait) g_sched_hooks.task_wait((int64_t)(intptr_t)t);
}

void rae_parallel_for(int64_t start, int64_t end, RaeParallelBody body, void* captures) {
  rae_pcheck_on();
  rae_pcheck_new_launch();
  if (end > start) body(captures, start, end);
}
