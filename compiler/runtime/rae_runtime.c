#include "rae_runtime.h"

#include <stdio.h>

static void rae_flush_stdout(void) {
  fflush(stdout);
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
