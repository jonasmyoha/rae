/* The scheduler's clock and the timers of sleeping tasks (included by
 * runtime_threads.c for both the threaded and the single-thread build) */

int64_t rae_sched_now_ns(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

/* The timers of sleeping tasks: a locked binary heap by deadline. Workers
 * take the due ones (Scheduler.rae) and park no longer than the next. */
typedef struct { int64_t deadline; RaeTask* task; } RaeTimer;
static pthread_mutex_t g_sched_timer_lock = PTHREAD_MUTEX_INITIALIZER;
static RaeTimer* g_sched_timers = NULL;
static int64_t g_sched_timer_count = 0;
static int64_t g_sched_timer_cap = 0;
static _Atomic int64_t g_sched_next_deadline = 0;   /* 0: no timer */

static void rae_sched_timer_push(int64_t deadline, RaeTask* task) {
  pthread_mutex_lock(&g_sched_timer_lock);
  if (g_sched_timer_count == g_sched_timer_cap) {
    g_sched_timer_cap = g_sched_timer_cap ? g_sched_timer_cap * 2 : 256;
    g_sched_timers = (RaeTimer*)realloc(g_sched_timers, (size_t)g_sched_timer_cap * sizeof(RaeTimer));
  }
  int64_t i = g_sched_timer_count++;
  while (i > 0 && g_sched_timers[(i - 1) / 2].deadline > deadline) {
    g_sched_timers[i] = g_sched_timers[(i - 1) / 2];
    i = (i - 1) / 2;
  }
  g_sched_timers[i].deadline = deadline;
  g_sched_timers[i].task = task;
  atomic_store(&g_sched_next_deadline, g_sched_timers[0].deadline);
  pthread_mutex_unlock(&g_sched_timer_lock);
}

/* The task of the earliest timer due by `now_ns`, or 0 */
int64_t rae_ext_Scheduler_timerDue(int64_t now_ns) {
  int64_t next = atomic_load(&g_sched_next_deadline);
  if (next == 0 || next > now_ns) return 0;
  pthread_mutex_lock(&g_sched_timer_lock);
  RaeTask* task = NULL;
  if (g_sched_timer_count > 0 && g_sched_timers[0].deadline <= now_ns) {
    task = g_sched_timers[0].task;
    RaeTimer last = g_sched_timers[--g_sched_timer_count];
    int64_t i = 0;
    for (;;) {
      int64_t child = 2 * i + 1;
      if (child >= g_sched_timer_count) break;
      if (child + 1 < g_sched_timer_count && g_sched_timers[child + 1].deadline < g_sched_timers[child].deadline) child++;
      if (g_sched_timers[child].deadline >= last.deadline) break;
      g_sched_timers[i] = g_sched_timers[child];
      i = child;
    }
    if (g_sched_timer_count > 0) g_sched_timers[i] = last;
  }
  atomic_store(&g_sched_next_deadline, g_sched_timer_count > 0 ? g_sched_timers[0].deadline : 0);
  pthread_mutex_unlock(&g_sched_timer_lock);
  return (int64_t)(intptr_t)task;
}

/* The earliest timer's deadline (monotonic ns), or 0 */
int64_t rae_ext_Scheduler_nextTimer(void) {
  return atomic_load(&g_sched_next_deadline);
}

int64_t rae_ext_Scheduler_nowNs(void) {
  return rae_sched_now_ns();
}

