/* One run of an app that logs through device_log.c, for device-log-test.sh:
 *   device_log_test <folder> <how it goes> */
#include "device_log.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The runtime's, when the runtime is not linked */
void (*rae_crash_last_call)(void) = NULL;

static void raw(const char* text) { write(STDERR_FILENO, text, strlen(text)); }

/* What the runtime's crash handler does: fragments to descriptor 2, then _exit */
static void on_crash(int sig) {
  raw("rae: abort in ");
  raw("test\n");
  raw("[rae crash] the last line\n");
  if (rae_crash_last_call) rae_crash_last_call();
  _exit(128 + sig);
}

static void* hammer(void* which) {
  char line[700];
  for (int i = 0; i < 2000; i++) {
    int kind = (int)(long)which;
    snprintf(line, sizeof(line), "thread %d line %d %0*d end\n", kind, i, 40 + i % 300, i);
    if (kind == 0) fputs(line, stdout);
    else if (kind == 1) fputs(line, stderr);
    else raw(line);
  }
  return NULL;
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const char* how = argv[2];
  signal(SIGABRT, on_crash);
  if (!device_log_start(argv[1], NULL)) return 3;
  device_log_identity("[run] app Test 1.0\n[run] binary UUID none\n");
  printf("the run %s\n", argc > 3 ? argv[3] : how);
  if (!strcmp(how, "mix")) {
    for (int i = 0; i < 200; i++) {
      printf("out %d\n", i * 3);
      fprintf(stderr, "err %d\n", i * 3 + 1);
      char text[64];
      snprintf(text, sizeof(text), "raw %d\n", i * 3 + 2);
      raw(text);
    }
    printf("half of");
    fflush(stdout);
    printf(" a line\n");
    raw("raw at the end\n");
  } else if (!strcmp(how, "kill")) {
    for (int i = 0; i < 1000; i++) printf("line %d\n", i);
    kill(getpid(), SIGKILL);
  } else if (!strcmp(how, "abort")) {
    raw("thread '<unnamed>' panicked at src/lib.rs:1:\nwhy it did\n");
    abort();
  } else if (!strcmp(how, "trap")) {
    raw("the assertion's words\n");
    __builtin_trap();
  } else if (!strcmp(how, "threads")) {
    pthread_t threads[3];
    for (long i = 0; i < 3; i++) pthread_create(&threads[i], NULL, hammer, (void*)i);
    for (int i = 0; i < 3; i++) pthread_join(threads[i], NULL);
  } else if (!strcmp(how, "long")) {
    for (int i = 0; i < 3000; i++) printf("long run line %d\n", i);
  }
  return 0;
}
