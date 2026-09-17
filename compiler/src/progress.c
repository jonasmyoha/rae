#include "progress.h"
#include "sys_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define PROGRESS_CELLS 20
#define PROGRESS_ACTIVITY_CELLS 10
#define PROGRESS_TICK_MS 200
/* After progress_hide() the ticker stays quiet this long, so a burst of
 * diagnostic lines is not interleaved with redraws. */
#define PROGRESS_QUIET_MS 400

static const char* const phase_names[PROGRESS_PHASES] = {
  "loading modules", "sema", "emitting C", "compiling C"
};

/* Before the first recorded build: the rough shape of a mid-sized example
 * at -O2 (the C compiler dominates). */
static const long long default_phase_ms[PROGRESS_PHASES] = { 1500, 2500, 2500, 6000 };
static const int default_modules = 60;

static struct {
  bool active;
  sys_mutex_t lock;
  sys_thread_t thread;
  bool stop;
  bool drawn;                 /* the two lines are currently on screen */
  long long quiet_until_ms;   /* no redraw before this (after a hide) */
  long long started_ms;
  ProgressPhase phase;
  ProgressPhase last_phase;   /* the pipeline ends after this one */
  long long phase_started_ms;
  long long phase_ms[PROGRESS_PHASES];       /* this build, completed phases */
  long long last_phase_ms[PROGRESS_PHASES];  /* the estimate (last build) */
  int last_modules;
  int modules_loaded;
  char record_path[4096];
} g;

static long long now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000 + (long long)tv.tv_usec / 1000;
}

static void read_record(void) {
  for (int i = 0; i < PROGRESS_PHASES; i++) g.last_phase_ms[i] = default_phase_ms[i];
  g.last_modules = default_modules;
  if (!g.record_path[0]) return;
  FILE* f = fopen(g.record_path, "r");
  if (!f) return;
  long long ms[PROGRESS_PHASES];
  int modules = 0;
  if (fscanf(f, "%lld %lld %lld %lld %d", &ms[0], &ms[1], &ms[2], &ms[3], &modules) == 5) {
    for (int i = 0; i < PROGRESS_PHASES; i++) if (ms[i] > 0) g.last_phase_ms[i] = ms[i];
    if (modules > 0) g.last_modules = modules;
  }
  fclose(f);
}

static void write_record(void) {
  if (!g.record_path[0]) return;
  FILE* f = fopen(g.record_path, "w");
  if (!f) return;  /* unwritable: the next build simply uses the defaults again */
  /* Phases this pipeline did not run (an emit-only build has no C compile)
   * keep their previous figure rather than being zeroed. */
  for (int i = 0; i < PROGRESS_PHASES; i++) {
    long long ms = i <= (int)g.last_phase ? g.phase_ms[i] : g.last_phase_ms[i];
    fprintf(f, "%lld ", ms);
  }
  fprintf(f, "%d\n", g.modules_loaded);
  fclose(f);
}

/* Estimated completion, 0..1. Finished phases count in full; the current one
 * by elapsed time against last build's duration (and by modules loaded while
 * loading), never past 97% so the bar cannot show done before it is. */
static double estimate(long long now) {
  long long total = 0;
  for (int i = 0; i <= (int)g.last_phase; i++) total += g.last_phase_ms[i];
  if (total <= 0) return 0.0;
  double done = 0.0;
  for (int i = 0; i < (int)g.phase; i++) done += (double)g.last_phase_ms[i];
  double frac = (double)(now - g.phase_started_ms) / (double)g.last_phase_ms[g.phase];
  if (g.phase == PROGRESS_LOAD && g.last_modules > 0) {
    double by_modules = (double)g.modules_loaded / (double)g.last_modules;
    if (by_modules > frac) frac = by_modules;
  }
  if (frac > 0.97) frac = 0.97;
  if (frac < 0.0) frac = 0.0;
  done += frac * (double)g.last_phase_ms[g.phase];
  return done / (double)total;
}

/* Called with the lock held. */
static void draw(long long now) {
  char activity[PROGRESS_ACTIVITY_CELLS + 1];
  int second = (int)((now - g.started_ms) / 1000) % (2 * PROGRESS_ACTIVITY_CELLS);
  for (int i = 0; i < PROGRESS_ACTIVITY_CELLS; i++) {
    bool dot = second < PROGRESS_ACTIVITY_CELLS ? i <= second
                                                : i > second - PROGRESS_ACTIVITY_CELLS;
    activity[i] = dot ? '.' : ' ';
  }
  activity[PROGRESS_ACTIVITY_CELLS] = '\0';

  char bar[PROGRESS_CELLS + 1];
  int filled = (int)(estimate(now) * PROGRESS_CELLS + 0.5);
  if (filled >= PROGRESS_CELLS) filled = PROGRESS_CELLS - 1;  /* full means done, and we are not */
  for (int i = 0; i < PROGRESS_CELLS; i++) bar[i] = i < filled ? '.' : ' ';
  bar[PROGRESS_CELLS] = '\0';

  /* Redraw in place: back up to the first line when the pair is already
   * there, clear each line to its end, leave the cursor after the bar. */
  if (g.drawn) fputs("\r\033[1A", stderr);
  fprintf(stderr, "%s\033[K\n[%s] %s\033[K", activity, bar, phase_names[g.phase]);
  fflush(stderr);
  g.drawn = true;
}

/* Called with the lock held. */
static void clear(void) {
  if (!g.drawn) return;
  fputs("\r\033[K\033[1A\033[K", stderr);
  fflush(stderr);
  g.drawn = false;
}

static void* ticker(void* arg) {
  (void)arg;
  for (;;) {
    sys_sleep_ms(PROGRESS_TICK_MS);
    sys_mutex_lock(&g.lock);
    if (g.stop) {
      sys_mutex_unlock(&g.lock);
      break;
    }
    long long now = now_ms();
    if (now >= g.quiet_until_ms) draw(now);
    sys_mutex_unlock(&g.lock);
  }
  return NULL;
}

void progress_begin(const char* record_path, ProgressPhase last_phase) {
  const char* env = getenv("RAE_PROGRESS");
  if (env && (strcmp(env, "off") == 0 || strcmp(env, "0") == 0)) return;
  if (!isatty(STDERR_FILENO)) return;
  memset(&g, 0, sizeof(g));
  if (record_path) snprintf(g.record_path, sizeof(g.record_path), "%s", record_path);
  g.last_phase = last_phase;
  read_record();
  g.started_ms = now_ms();
  g.phase = PROGRESS_LOAD;
  g.phase_started_ms = g.started_ms;
  if (!sys_mutex_init(&g.lock)) return;
  g.active = true;
  if (!sys_thread_create(&g.thread, ticker, NULL)) {
    g.active = false;
    sys_mutex_destroy(&g.lock);
  }
}

void progress_phase(ProgressPhase phase) {
  if (!g.active) return;
  sys_mutex_lock(&g.lock);
  long long now = now_ms();
  g.phase_ms[g.phase] = now - g.phase_started_ms;
  g.phase = phase;
  g.phase_started_ms = now;
  sys_mutex_unlock(&g.lock);
}

void progress_module_loaded(int loaded) {
  if (!g.active) return;
  sys_mutex_lock(&g.lock);
  g.modules_loaded = loaded;
  sys_mutex_unlock(&g.lock);
}

void progress_hide(void) {
  if (!g.active) return;
  sys_mutex_lock(&g.lock);
  clear();
  g.quiet_until_ms = now_ms() + PROGRESS_QUIET_MS;
  sys_mutex_unlock(&g.lock);
}

void progress_end(bool ok) {
  if (!g.active) return;
  sys_mutex_lock(&g.lock);
  g.stop = true;
  g.phase_ms[g.phase] = now_ms() - g.phase_started_ms;
  clear();
  sys_mutex_unlock(&g.lock);
  sys_thread_join(g.thread);
  if (ok) write_record();
  sys_mutex_destroy(&g.lock);
  g.active = false;
}
