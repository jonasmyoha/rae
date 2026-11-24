/* The log file of an app on a phone, where nobody sees stdout: stdout and
 * stderr go to <folder>/rae.log, pulled off the phone afterwards.
 *
 *   rae.log                  this run
 *   rae.previous.log         the run before it
 *   rae.previous2.log ...    and the ones before that (DEVICE_LOG_KEPT in all)
 *   rae.earlier.log          this run's earlier part, once rae.log was full
 *   rae.previous.earlier.log the same of the run before it
 *
 * A start renames, never truncates: the log of a run that crashed is still
 * there after the app is started again.
 *
 * Every line begins with its local time to the millisecond ("12:26:22.364 "),
 * so it can be matched with the system's own logs; the first lines say the
 * date and the time zone, and a line says so when the date or the zone changes.
 *
 * How the lines get there, whole, in order and at once:
 *   - stdout and stderr are stdio streams of our own (funopen): a line is
 *     written to the file by the thread that prints it, before the print
 *     returns. A line that was printed survives a SIGKILL.
 *   - what writes to the descriptors 1 and 2 themselves (a Rust panic, NSLog,
 *     the crash handler's backtrace) goes into a pipe. A thread reads it, and
 *     so does every print before its own line, so the order holds. A crash
 *     reads what is left (rae_crash_last_call), as does exit(). What an app
 *     wrote raw in the last moments before a SIGKILL may be lost. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#include "device_log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEVICE_LOG_KEPT 3              /* runs kept before this one */
#define DEVICE_LOG_MAX_KB 4096         /* a part's size; RAE_LOG_MAX_KB for a test */

/* The runtime's crash handler calls it last (runtime_core_memory.c) */
extern void (*rae_crash_last_call)(void);

enum { FROM_STDOUT, FROM_STDERR, FROM_RAW, FROM_COUNT };

static struct {
  pthread_mutex_t lock;
  int file;
  int pipe[2];
  char folder[900];
  char identity[2048];
  long long written, most;
  int line_open, line_from;            /* a line not ended yet, and whose it is */
  time_t second;                       /* what the two below were asked for */
  long offset;                         /* local time less UTC, s */
  long day;                            /* the local day the log said last */
  long zone;                           /* and the offset */
  void (*line)(const char* text);
  struct { char text[2048]; size_t used; } lines[FROM_COUNT];
} device_log = {.lock = PTHREAD_MUTEX_INITIALIZER, .file = -1, .pipe = {-1, -1}};

static void write_file(const char* text, size_t length) {
  device_log.written += (long long)length;
  while (length > 0) {
    ssize_t wrote = write(device_log.file, text, length);
    if (wrote < 0) {
      if (errno == EINTR) continue;
      return;
    }
    text += wrote;
    length -= (size_t)wrote;
  }
}

static char* put_digits(char* at, long value, int digits) {
  for (int i = digits - 1; i >= 0; i--) {
    at[i] = (char)('0' + value % 10);
    value /= 10;
  }
  return at + digits;
}

/* "12:26:22.364 ", by hand: a crash handler may not call snprintf */
static size_t stamp(char out[16], const struct timespec* now) {
  long day_second = (long)(((long long)now->tv_sec + device_log.offset) % 86400);
  char* at = put_digits(out, day_second / 3600, 2);
  *at++ = ':';
  at = put_digits(at, day_second / 60 % 60, 2);
  *at++ = ':';
  at = put_digits(at, day_second % 60, 2);
  *at++ = '.';
  at = put_digits(at, now->tv_nsec / 1000000, 3);
  *at++ = ' ';
  return (size_t)(at - out);
}

static void path_of(char* out, size_t size, const char* name) {
  snprintf(out, size, "%s/%s", device_log.folder, name);
}

static void rename_log(const char* from, const char* to) {
  char from_path[1024], to_path[1024];
  path_of(from_path, sizeof(from_path), from);
  path_of(to_path, sizeof(to_path), to);
  rename(from_path, to_path);
}

static void previous_name(char* out, size_t size, int back) {
  if (back == 1) snprintf(out, size, "rae.previous.log");
  else snprintf(out, size, "rae.previous%d.log", back);
}

/* O_APPEND: whatever else opens the file, nothing overwrites a line */
static int open_log(void) {
  char path[1024];
  path_of(path, sizeof(path), "rae.log");
  return open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
}

/* A line of the log's own, whole and stamped; the lock is held */
static void say(const struct timespec* now, const char* format, ...) __attribute__((format(printf, 2, 3)));
static void say(const struct timespec* now, const char* format, ...) {
  char text[512], time_text[16];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(text, sizeof(text) - 1, format, args);
  va_end(args);
  if (length < 0) return;
  if (length > (int)sizeof(text) - 2) length = (int)sizeof(text) - 2;
  if (device_log.line_open) {
    write_file("\n", 1);
    device_log.line_open = 0;
  }
  if (device_log.line) device_log.line(text);
  text[length++] = '\n';
  write_file(time_text, stamp(time_text, now));
  write_file(text, (size_t)length);
}

/* The local day and the time zone, said at first and whenever either changes
 * (midnight, summer time, a journey): the lines' times alone do not tell */
static void say_date(const struct timespec* now, const char* lead) {
  struct tm local;
  char date[32], zone[32];
  localtime_r(&now->tv_sec, &local);
  strftime(date, sizeof(date), "%Y-%m-%d", &local);
  strftime(zone, sizeof(zone), "UTC%z (%Z)", &local);
  say(now, "[run] %s %s, local time %s", lead, date, zone);
}

/* Asked once a second and never in a crash */
static void learn_time(const struct timespec* now) {
  if (now->tv_sec == device_log.second) return;
  struct tm local;
  localtime_r(&now->tv_sec, &local);
  device_log.second = now->tv_sec;
  device_log.offset = local.tm_gmtoff;
  long day = (long)(((long long)now->tv_sec + device_log.offset) / 86400);
  if (device_log.day != 0 && (day != device_log.day || device_log.offset != device_log.zone)) {
    device_log.day = day;
    device_log.zone = device_log.offset;
    say_date(now, "the date is now");
  }
  device_log.day = day;
  device_log.zone = device_log.offset;
}

static void say_identity(const struct timespec* now) {
  const char* at = device_log.identity;
  while (*at) {
    size_t length = strcspn(at, "\n");
    say(now, "%.*s", (int)length, at);
    at += length + (at[length] == '\n');
  }
}

/* rae.log is full: it becomes rae.earlier.log (an earlier one is dropped) and
 * a new one goes on, so a run keeps its last one or two parts */
static void next_part(const struct timespec* now) {
  int file = -1;
  rename_log("rae.log", "rae.earlier.log");
  file = open_log();
  if (file < 0) {
    device_log.written = 0;            /* go on in the renamed file */
    return;
  }
  dup2(file, device_log.file);
  close(file);
  device_log.written = 0;
  say_date(now, "this run's log goes on, what came before is in rae.earlier.log;");
  say_identity(now);
}

static void feed_line(int from, const char* text, size_t length) {
  for (size_t i = 0; i < length; i++) {
    size_t* used = &device_log.lines[from].used;
    char* line = device_log.lines[from].text;
    if (text[i] == '\n' || *used == sizeof(device_log.lines[from].text) - 1) {
      line[*used] = 0;
      device_log.line(line);
      *used = 0;
      if (text[i] == '\n') continue;
    }
    line[(*used)++] = text[i];
  }
}

/* Text of one of the three, as it came, into the file; the lock is held. The
 * time goes before the first byte of a line. A line another one's text would
 * break is ended first. */
static void put(int from, const char* text, size_t length, int crashing) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  if (!crashing) learn_time(&now);
  if (device_log.line) feed_line(from, text, length);
  while (length > 0) {
    if (device_log.line_open && device_log.line_from != from) {
      write_file("\n", 1);
      device_log.line_open = 0;
    }
    if (!device_log.line_open && !crashing && device_log.written >= device_log.most) next_part(&now);
    const char* end = memchr(text, '\n', length);
    size_t part = end ? (size_t)(end - text) + 1 : length;
    if (device_log.line_open || part + 16 > 1024) {
      if (!device_log.line_open) {
        char time_text[16];
        write_file(time_text, stamp(time_text, &now));
      }
      write_file(text, part);
    } else {
      /* Time and line in one write */
      char whole[1024 + 16];
      size_t used = stamp(whole, &now);
      memcpy(whole + used, text, part);
      write_file(whole, used + part);
    }
    device_log.line_open = end == NULL;
    device_log.line_from = from;
    text += part;
    length -= part;
  }
}

/* What was written to the descriptors 1 and 2 since the last look */
static void drain(int crashing) {
  char chunk[1024];
  for (;;) {
    ssize_t got = read(device_log.pipe[0], chunk, sizeof(chunk));
    if (got > 0) put(FROM_RAW, chunk, (size_t)got, crashing);
    else if (got < 0 && errno == EINTR) continue;
    else return;
  }
}

static int print_from(int from, const char* text, int length) {
  if (length <= 0) return length;
  pthread_mutex_lock(&device_log.lock);
  drain(0);
  put(from, text, (size_t)length, 0);
  pthread_mutex_unlock(&device_log.lock);
  return length;
}

static int write_stdout(void* unused, const char* text, int length) {
  (void)unused;
  return print_from(FROM_STDOUT, text, length);
}

static int write_stderr(void* unused, const char* text, int length) {
  (void)unused;
  return print_from(FROM_STDERR, text, length);
}

static void* read_pipe(void* unused) {
  (void)unused;
  for (;;) {
    struct pollfd wait = {.fd = device_log.pipe[0], .events = POLLIN};
    if (poll(&wait, 1, -1) < 0 && errno != EINTR) return NULL;
    pthread_mutex_lock(&device_log.lock);
    drain(0);
    pthread_mutex_unlock(&device_log.lock);
  }
}

static void drain_at_exit(void) {
  fflush(stdout);
  pthread_mutex_lock(&device_log.lock);
  drain(0);
  pthread_mutex_unlock(&device_log.lock);
}

/* In a crash. The lock may be held by the thread that crashed, or by one that
 * will never run again: a short wait, then without it. */
static void drain_in_crash(void) {
  int locked = 0;
  for (int i = 0; i < 50 && !locked; i++) {
    locked = pthread_mutex_trylock(&device_log.lock) == 0;
    if (!locked) {
      struct timespec wait = {0, 1000000};
      nanosleep(&wait, NULL);
    }
  }
  drain(1);
  if (locked) pthread_mutex_unlock(&device_log.lock);
}

/* A trap (Rust's abort, a failed assertion of Swift or the OS): not one of the
 * runtime's crash signals, and the system's crash report should stay, so the
 * trap happens again once the rest is read */
static void on_trap(int signal_number) {
  signal(signal_number, SIG_DFL);
  drain_in_crash();
}

int device_log_start(const char* folder, void (*line)(const char* text)) {
  if (!folder || !*folder || device_log.file >= 0) return 0;
  snprintf(device_log.folder, sizeof(device_log.folder), "%s", folder);
  device_log.line = line;
  device_log.most = DEVICE_LOG_MAX_KB * 1024LL;
  const char* most = getenv("RAE_LOG_MAX_KB");
  if (most && atoll(most) > 0) device_log.most = atoll(most) * 1024;

  for (int back = DEVICE_LOG_KEPT; back >= 1; back--) {
    char from[64], to[64];
    if (back == 1) snprintf(from, sizeof(from), "rae.log");
    else previous_name(from, sizeof(from), back - 1);
    previous_name(to, sizeof(to), back);
    rename_log(from, to);
  }
  /* The last run's earlier part, or none of it when the last run had one part */
  char stale[1024];
  path_of(stale, sizeof(stale), "rae.previous.earlier.log");
  unlink(stale);
  rename_log("rae.earlier.log", "rae.previous.earlier.log");

  int file = open_log();
  if (file < 0) return 0;
  FILE* out = funopen(NULL, NULL, write_stdout, NULL, NULL);
  FILE* err = funopen(NULL, NULL, write_stderr, NULL, NULL);
  if (!out || !err || pipe(device_log.pipe) != 0) {
    close(file);
    return 0;
  }
  device_log.file = file;
  setvbuf(out, NULL, _IOLBF, 4096);
  setvbuf(err, NULL, _IONBF, 0);
  fcntl(device_log.pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(device_log.pipe[0], F_SETFD, FD_CLOEXEC);
  fflush(stdout);
  fflush(stderr);

  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  learn_time(&now);
  say_date(&now, "started");

  pthread_t thread;
  if (pthread_create(&thread, NULL, read_pipe, NULL) == 0) {
    pthread_detach(thread);
    dup2(device_log.pipe[1], STDOUT_FILENO);
    dup2(device_log.pipe[1], STDERR_FILENO);
    close(device_log.pipe[1]);
  } else {
    /* No reader: raw writes go straight to the file, without a time */
    dup2(file, STDOUT_FILENO);
    dup2(file, STDERR_FILENO);
  }
  stdout = out;
  stderr = err;
  atexit(drain_at_exit);
  rae_crash_last_call = drain_in_crash;
  signal(SIGTRAP, on_trap);
  return 1;
}

void device_log_identity(const char* lines) {
  pthread_mutex_lock(&device_log.lock);
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  snprintf(device_log.identity, sizeof(device_log.identity), "%s", lines);
  if (device_log.file >= 0) say_identity(&now);
  pthread_mutex_unlock(&device_log.lock);
}
