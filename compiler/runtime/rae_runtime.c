#include "rae_runtime.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static void rae_flush_stdout(void) {
  fflush(stdout);
}

static int64_t g_tick_counter = 0;

int64_t nextTick(void) {
  return ++g_tick_counter;
}

void sleepMs(int64_t ms) {
  if (ms > 0) {
    usleep((useconds_t)ms * 1000);
  }
}

void rae_log_cstr(const char* text) {
  if (!text) {
    fputs("(null)\n", stdout);
    rae_flush_stdout();
    return;
  }
  fputs(text, stdout);
  fputc('\n', stdout);
  rae_flush_stdout();
}

void rae_log_stream_cstr(const char* text) {
  if (!text) {
    return;
  }
  fputs(text, stdout);
  rae_flush_stdout();
}

void rae_log_i64(int64_t value) {
  printf("%lld\n", (long long)value);
  rae_flush_stdout();
}

void rae_log_stream_i64(int64_t value) {
  printf("%lld", (long long)value);
  rae_flush_stdout();
}

const char* rae_str_concat(const char* a, const char* b) {
  if (!a) a = "";
  if (!b) b = "";
  size_t len_a = strlen(a);
  size_t len_b = strlen(b);
  char* result = malloc(len_a + len_b + 1);
  if (result) {
    strcpy(result, a);
    strcat(result, b);
  }
  return result;
}

const char* rae_str_i64(int64_t v) {
  char* buffer = malloc(32);
  if (buffer) {
    sprintf(buffer, "%lld", (long long)v);
  }
  return buffer;
}

const char* rae_str_cstr(const char* s) {
  // Return copy or identity? Concatenation frees nothing?
  // Concatenation does NOT free inputs.
  // So returning s is safe if s is managed elsewhere.
  // But if s is literal, it's fine.
  // If s is result of concat (allocated), it's fine.
  return s;
}

int64_t nextTick(void) {
