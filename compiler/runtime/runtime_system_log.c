/* Time, bare spawn, ad-hoc JSON bridge, and logging helpers. Mixed kernel and temporary bridge code.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

int64_t rae_ext_nextTick(void) {
  return atomic_fetch_add_explicit(&g_tick_counter, 1, memory_order_relaxed) + 1;
}

int64_t rae_ext_nowMs(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

int64_t rae_ext_nowNs(void) {
#ifdef __APPLE__
  /* Read per call, not cached in a static: any thread may ask for the time,
   * and a lazily filled static is a data race. mach_timebase_info only
   * copies two constants. */
  mach_timebase_info_data_t timebase;
  mach_timebase_info(&timebase);
  return (int64_t)((mach_absolute_time() * timebase.numer) / timebase.denom);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

void rae_ext_rae_sleep(int64_t ms) {
  if (ms > 0) {
    usleep((useconds_t)ms * 1000);
  }
}

/* One nanosleep (lib/Time.rae waitUntil): 0, or -errno (EINTR when a signal
 * cut it short; the caller re-reads the clock). Platform reason: sleep(ms:)
 * takes whole milliseconds, and a 30 Hz tick (33.333 ms) needs finer. */
int64_t rae_ext_Time_sleepNs(int64_t ns) {
  if (ns <= 0) return 0;
  struct timespec wait = { (time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL) };
  return nanosleep(&wait, NULL) == 0 ? 0 : -errno;
}


void rae_spawn(void* (*func)(void*), void* data) {
#ifdef _WIN32
    CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, data, 0, NULL);
#elif defined(__wasm__) && !defined(RAE_WASM_THREADS)
    (void)func; (void)data;  /* single-threaded wasip1: no OS threads */
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, func, data) == 0) {
        pthread_detach(thread);
    }
#endif
}

/* The one thing `log` / `logS` need from C: write `text` to stdout, then a
 * newline when `newline` is set, and flush. Everything that turns a value into
 * text is Rae (lib/core/AnyText.rae, the generated formatters;
 * docs/runtime-c-audit.md row 12). Platform reason: writing to stdout. */
void rae_ext_rae_log_write(rae_String text, rae_Bool newline) {
  if (text.data && text.len > 0) fwrite(text.data, 1, (size_t)text.len, stdout);
  if (newline) fputc('\n', stdout);
  rae_flush_stdout();
}

