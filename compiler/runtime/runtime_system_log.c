/* Time, bare spawn, ad-hoc JSON bridge, and logging helpers. Mixed kernel and temporary bridge code.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

int64_t rae_ext_nextTick(void) {
  return atomic_fetch_add_explicit(&g_tick_counter, 1, memory_order_relaxed) + 1;
}

static int64_t rae_real_now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

/* A test clock: with RAE_NOW set, the wall clock (nowMs, and everything that
 * reads the date or the time of day from it) starts at that moment and runs
 * on from there, so a test that depends on the time of day gives the same
 * answer at any hour. RAE_NOW is a local time, 2026-11-03T10:00 (seconds
 * optional), or epoch milliseconds. A value that is not a time stops the
 * program at its first clock read, naming it: a test must never run on the
 * real clock while it believes it runs on a fixed one. The monotonic clock
 * (nowNs, frame timing) is not touched. */
static pthread_once_t g_now_once = PTHREAD_ONCE_INIT;
static int64_t g_now_offset_ms = 0;

/* RAE_NOW as epoch ms, or -1 when it is not a time */
static int64_t rae_now_parse(const char* text) {
  size_t len = strlen(text);
  int digits = len > 0;
  for (size_t i = 0; i < len; i++) {
    if (text[i] < '0' || text[i] > '9') digits = 0;
  }
  if (digits) return (int64_t)strtoll(text, NULL, 10);
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, used = 0;
  if (sscanf(text, "%4d-%2d-%2dT%2d:%2d%n", &y, &mo, &d, &h, &mi, &used) != 5) return -1;
  if (text[used] == ':') {
    int more = 0;
    if (sscanf(text + used, ":%2d%n", &sec, &more) != 1) return -1;
    used += more;
  }
  if (text[used] != '\0') return -1;
  struct tm tm;
  memset(&tm, 0, sizeof tm);
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = sec;
  tm.tm_isdst = -1;
  time_t t = mktime(&tm);
  /* mktime moves a day or an hour that does not exist (2026-02-30): refused */
  if (t == (time_t)-1 || tm.tm_year != y - 1900 || tm.tm_mon != mo - 1 || tm.tm_mday != d ||
      tm.tm_hour != h || tm.tm_min != mi || tm.tm_sec != sec) return -1;
  return (int64_t)t * 1000;
}

static void rae_now_setup(void) {
  const char* text = getenv("RAE_NOW");
  if (!text || !*text) return;
  int64_t start = rae_now_parse(text);
  if (start < 0) {
    fprintf(stderr, "rae: RAE_NOW='%s' is not a time: write a local time like 2026-11-03T10:00 (seconds optional), or epoch milliseconds\n", text);
    exit(2);
  }
  g_now_offset_ms = start - rae_real_now_ms();
}

int64_t rae_ext_nowMs(void) {
  pthread_once(&g_now_once, rae_now_setup);
  return rae_real_now_ms() + g_now_offset_ms;
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

/* The local time zone's offset from UTC, in seconds east, at `epochSeconds`
 * (lib/Time.rae localUtcOffsetSeconds; daylight saving included). Platform
 * reason: the zone database and the user's zone setting are the OS's.
 * WASM has no zone: 0 (UTC). */
int64_t rae_ext_Time_localOffsetSeconds(int64_t epochSeconds) {
#if defined(__wasm__)
  (void)epochSeconds;
  return 0;
#elif defined(_WIN32)
  time_t when = (time_t)epochSeconds;
  struct tm local;
  struct tm utc;
  if (localtime_s(&local, &when) != 0 || gmtime_s(&utc, &when) != 0) return 0;
  return (int64_t)difftime(mktime(&local), mktime(&utc)) + (local.tm_isdst > 0 ? 3600 : 0);
#else
  time_t when = (time_t)epochSeconds;
  struct tm local;
  if (localtime_r(&when, &local) == NULL) return 0;
  return (int64_t)local.tm_gmtoff;
#endif
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
  RAE_REALTIME_GUARD("rae_ext_rae_log_write", "log");
  if (text.data && text.len > 0) fwrite(text.data, 1, (size_t)text.len, stdout);
  if (newline) fputc('\n', stdout);
  rae_flush_stdout();
}

