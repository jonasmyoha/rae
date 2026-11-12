/* runtime_realtime.c — the run-time real-time check
 * (docs/realtime-language-design.md §5.3, lib/Realtime.rae).
 *
 * A thread marks itself real-time (markRealtimeThread). With
 * RAE_REALTIME_CHECK=1, the runtime's allocation, free, lock and log entry
 * points call RAE_REALTIME_GUARD, which counts a call made on a marked thread.
 * At exit the counts are printed to stderr, one line per entry point, and the
 * process exits with code 70, so a test that ran real-time code which
 * allocated, freed, locked or logged fails.
 *
 * Recording a call must itself be real-time-safe: it never allocates, locks or
 * prints. Each distinct (entry point, detail) pair claims one slot of a fixed
 * table with a compare-and-swap; further calls add to its counter. Past the
 * table's size, calls are counted in `overflow`.
 *
 * Platform reason for C: the check sits inside the runtime's own allocator and
 * lock wrappers, which are C, and the mark is a thread-local variable. */
#define RAE_REALTIME_SLOTS 32

typedef struct {
  const char* _Atomic entry;   /* the runtime function, a static string */
  const char* _Atomic detail;  /* what it was for, a static string or NULL */
  _Atomic int64_t count;
} RaeRealtimeSlot;

static RaeRealtimeSlot g_rae_realtime_slots[RAE_REALTIME_SLOTS];
static _Atomic int64_t g_rae_realtime_overflow;

static void rae_realtime_violation(const char* entry, const char* detail) {
  for (int i = 0; i < RAE_REALTIME_SLOTS; i++) {
    RaeRealtimeSlot* slot = &g_rae_realtime_slots[i];
    const char* held = atomic_load_explicit(&slot->entry, memory_order_acquire);
    if (held == NULL) {
      /* Claim the slot. The winner then publishes `detail`; a thread that
       * reads the slot before that sees no match and uses another slot, which
       * at worst splits one pair's count over two lines. */
      const char* expected = NULL;
      if (atomic_compare_exchange_strong_explicit(&slot->entry, &expected, entry,
                                                  memory_order_acq_rel, memory_order_acquire)) {
        atomic_store_explicit(&slot->detail, detail, memory_order_release);
        atomic_fetch_add_explicit(&slot->count, 1, memory_order_relaxed);
        return;
      }
      held = expected;
    }
    if (held == entry && atomic_load_explicit(&slot->detail, memory_order_acquire) == detail) {
      atomic_fetch_add_explicit(&slot->count, 1, memory_order_relaxed);
      return;
    }
  }
  atomic_fetch_add_explicit(&g_rae_realtime_overflow, 1, memory_order_relaxed);
}

static void rae_realtime_check_report(void) {
  int64_t total = atomic_load(&g_rae_realtime_overflow);
  for (int i = 0; i < RAE_REALTIME_SLOTS; i++) total += atomic_load(&g_rae_realtime_slots[i].count);
  if (total == 0) return;
  rae_flush_stdout();
  fprintf(stderr, "[realtime-check] %lld call(s) on a real-time thread that may allocate, free, lock or log:\n",
          (long long)total);
  for (int i = 0; i < RAE_REALTIME_SLOTS; i++) {
    RaeRealtimeSlot* slot = &g_rae_realtime_slots[i];
    const char* entry = atomic_load(&slot->entry);
    if (!entry) continue;
    const char* detail = atomic_load(&slot->detail);
    if (detail) {
      fprintf(stderr, "  %s (%s): %lld\n", entry, detail, (long long)atomic_load(&slot->count));
    } else {
      fprintf(stderr, "  %s: %lld\n", entry, (long long)atomic_load(&slot->count));
    }
  }
  int64_t overflow = atomic_load(&g_rae_realtime_overflow);
  if (overflow > 0) fprintf(stderr, "  (other entry points): %lld\n", (long long)overflow);
  fflush(stderr);
  fflush(NULL);
  _exit(70);
}

/* The exit report is registered here, at startup, and never by a mark: a
 * mark runs in real-time code, and atexit takes a lock (RealtimeSanitizer
 * found the earlier first-mark registration). It is registered after the
 * RAE_MEM_STATS report (whose constructor has an earlier priority), so it
 * runs before it at exit (atexit runs in reverse) and the memory report never
 * comes between a program's output and this one. */
__attribute__((constructor))
static void rae_install_realtime_check(void) {
  const char* value = getenv("RAE_REALTIME_CHECK");
  if (value && value[0] && value[0] != '0') {
    g_rae_realtime_check = 1;
    atexit(rae_realtime_check_report);
  }
}

/* lib/Realtime.rae: mark or unmark the calling thread. One store. */
void rae_ext_Realtime_markThread(rae_Bool realtime) {
  g_rae_realtime_thread = realtime ? 1 : 0;
}

rae_Bool rae_ext_Realtime_threadIsMarked(void) {
  return g_rae_realtime_thread != 0;
}

/* Whether RAE_REALTIME_REPORT asks for the time recorders' reports. */
rae_Bool rae_ext_Realtime_reportWanted(void) {
  const char* value = getenv("RAE_REALTIME_REPORT");
  return value && value[0] && value[0] != '0';
}

/* ---- real-time priority for a `spawn thread` (lib/Realtime.rae
 * promoteToRealtime, docs/realtime-language-design.md §6.6.1) -------------
 * Platform reason: the scheduler calls themselves. Each function makes ONE
 * call on the calling thread and answers its result; which call, the values
 * and what a failure means are Rae's. */
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>

/* The Mach timebase: ticks = nanoseconds * denominator / numerator */
int64_t rae_ext_Realtime_timebaseNumerator(void) {
  mach_timebase_info_data_t timebase;
  return mach_timebase_info(&timebase) == KERN_SUCCESS ? (int64_t)timebase.numer : 0;
}

int64_t rae_ext_Realtime_timebaseDenominator(void) {
  mach_timebase_info_data_t timebase;
  return mach_timebase_info(&timebase) == KERN_SUCCESS ? (int64_t)timebase.denom : 0;
}

/* THREAD_TIME_CONSTRAINT_POLICY for the calling thread; 1 when it took */
int64_t rae_ext_Realtime_setTimeConstraint(int64_t periodTicks, int64_t computationTicks,
                                           int64_t constraintTicks) {
  thread_time_constraint_policy_data_t policy;
  policy.period = (uint32_t)periodTicks;
  policy.computation = (uint32_t)computationTicks;
  policy.constraint = (uint32_t)constraintTicks;
  policy.preemptible = 1;
  return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                           (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
}

/* Back to THREAD_STANDARD_POLICY; 1 when it took */
int64_t rae_ext_Realtime_setStandardPolicy(void) {
  thread_standard_policy_data_t policy;
  return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_STANDARD_POLICY,
                           (thread_policy_t)&policy, THREAD_STANDARD_POLICY_COUNT) == KERN_SUCCESS;
}
#elif defined(_WIN32)
#include <windows.h>
#include <avrt.h>
#if defined(_MSC_VER)
#pragma comment(lib, "avrt")
#endif

/* MMCSS "Pro Audio" for the calling thread: its handle, 0 when it failed */
int64_t rae_ext_Realtime_mmcssBegin(void) {
  DWORD taskIndex = 0;
  return (int64_t)(intptr_t)AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
}

int64_t rae_ext_Realtime_mmcssEnd(int64_t handle) {
  return AvRevertMmThreadCharacteristics((HANDLE)(intptr_t)handle) ? 1 : 0;
}
#elif defined(__ANDROID__)
#include <sys/resource.h>
#include <unistd.h>

/* The calling thread's nice value (an app cannot get SCHED_FIFO); 1 when it took */
int64_t rae_ext_Realtime_setNice(int64_t value) {
  return setpriority(PRIO_PROCESS, (id_t)gettid(), (int)value) == 0;
}
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>

/* SCHED_FIFO at `priority` for the calling thread (needs RLIMIT_RTPRIO); 0 or
 * the error number */
int64_t rae_ext_Realtime_setFifo(int64_t priority) {
  struct sched_param parameter;
  parameter.sched_priority = (int)priority;
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameter);
}

/* Back to SCHED_OTHER; 0 or the error number */
int64_t rae_ext_Realtime_setOther(void) {
  struct sched_param parameter;
  parameter.sched_priority = 0;
  return pthread_setschedparam(pthread_self(), SCHED_OTHER, &parameter);
}
#endif
