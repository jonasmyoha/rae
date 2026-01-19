#include "rae_runtime.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>

static void rae_flush_stdout(void) {
  fflush(stdout);
}

static int64_t g_tick_counter = 0;

int64_t nextTick(void) {
  return ++g_tick_counter;
}

int64_t rae_time_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

void sleepMs(int64_t ms) {
  if (ms > 0) {
    usleep((useconds_t)ms * 1000);
  }
}

void rae_log_any(RaeAny value) {
  rae_log_stream_any(value);
  printf("\n");
  rae_flush_stdout();
}

void rae_log_stream_any(RaeAny value) {
  switch (value.type) {
    case RAE_TYPE_INT: printf("%lld", (long long)value.as.i); break;
    case RAE_TYPE_FLOAT: printf("%g", value.as.f); break;
    case RAE_TYPE_BOOL: printf("%s", value.as.b ? "true" : "false"); break;
    case RAE_TYPE_STRING: printf("%s", value.as.s ? value.as.s : "(null)"); break;
    case RAE_TYPE_CHAR: rae_log_stream_char(value.as.i); break;
    case RAE_TYPE_ID: printf("Id(%lld)", (long long)value.as.i); break;
    case RAE_TYPE_KEY: printf("Key(\"%s\")", value.as.s ? value.as.s : "(null)"); break;
    case RAE_TYPE_LIST: printf("[...]"); break;
    case RAE_TYPE_BUFFER: printf("#(...)"); break;
    case RAE_TYPE_NONE: printf("none"); break;
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

void rae_log_bool(int8_t value) {
  printf("%s\n", value ? "true" : "false");
  rae_flush_stdout();
}

void rae_log_stream_bool(int8_t value) {
  printf("%s", value ? "true" : "false");
  rae_flush_stdout();
}

void rae_log_char(int64_t value) {
  rae_log_stream_char(value);
  printf("\n");
  rae_flush_stdout();
}

void rae_log_stream_char(int64_t value) {
  if (value < 0x80) {
    printf("%c", (char)value);
  } else if (value < 0x800) {
    printf("%c%c", (char)(0xC0 | (value >> 6)), (char)(0x80 | (value & 0x3F)));
  } else if (value < 0x10000) {
    printf("%c%c%c", (char)(0xE0 | (value >> 12)), (char)(0x80 | ((value >> 6) & 0x3F)), (char)(0x80 | (value & 0x3F)));
  } else {
    printf("%c%c%c%c", (char)(0xF0 | (value >> 18)), (char)(0x80 | ((value >> 12) & 0x3F)), (char)(0x80 | ((value >> 6) & 0x3F)), (char)(0x80 | (value & 0x3F)));
  }
  rae_flush_stdout();
}

void rae_log_id(int64_t value) {
  printf("%lld\n", (long long)value);
  rae_flush_stdout();
}

void rae_log_stream_id(int64_t value) {
  printf("%lld", (long long)value);
  rae_flush_stdout();
}

void rae_log_key(const char* value) {
  printf("%s\n", value ? value : "(null)");
  rae_flush_stdout();
}

void rae_log_stream_key(const char* value) {
  printf("%s", value ? value : "(null)");
  rae_flush_stdout();
}

void rae_log_float(double value) {
  printf("%g\n", value);
  rae_flush_stdout();
}

void rae_log_stream_float(double value) {
  printf("%g", value);
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

const char* rae_str_f64(double v) {
  char* buffer = malloc(32);
  if (buffer) {
    sprintf(buffer, "%g", v);
  }
  return buffer;
}

const char* rae_str_bool(int8_t v) {
  return v ? "true" : "false";
}

const char* rae_str_char(int64_t v) {
  char* buffer = malloc(5);
  if (buffer) {
    if (v < 0x80) {
      buffer[0] = (char)v;
      buffer[1] = '\0';
    } else if (v < 0x800) {
      buffer[0] = (char)(0xC0 | (v >> 6));
      buffer[1] = (char)(0x80 | (v & 0x3F));
      buffer[2] = '\0';
    } else if (v < 0x10000) {
      buffer[0] = (char)(0xE0 | (v >> 12));
      buffer[1] = (char)(0x80 | ((v >> 6) & 0x3F));
      buffer[2] = (char)(0x80 | (v & 0x3F));
      buffer[3] = '\0';
    } else {
      buffer[0] = (char)(0xF0 | (v >> 18));
      buffer[1] = (char)(0x80 | ((v >> 12) & 0x3F));
      buffer[2] = (char)(0x80 | ((v >> 6) & 0x3F));
      buffer[3] = (char)(0x80 | (v & 0x3F));
      buffer[4] = '\0';
    }
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

static uint64_t g_rae_random_state = 0x123456789ABCDEF0ULL;

void rae_seed(int64_t seed) {
  g_rae_random_state = (uint64_t)seed;
}

static uint32_t rae_next_u32(void) {
  g_rae_random_state = g_rae_random_state * 6364136223846793005ULL + 1;
  return (uint32_t)(g_rae_random_state >> 32);
}

double rae_random(void) {
  return (double)rae_next_u32() / (double)4294967295.0;
}

int64_t rae_random_int(int64_t min, int64_t max) {
  if (min >= max) return min;
  uint64_t range = (uint64_t)(max - min + 1);
  return min + (int64_t)(rae_next_u32() % range);
}

void* rae_buf_alloc(int64_t count, int64_t elem_size) {
  if (count <= 0) return NULL;
  return calloc((size_t)count, (size_t)elem_size);
}

void rae_buf_free(void* buf) {
  if (buf) free(buf);
}

void* rae_buf_resize(void* buf, int64_t new_count, int64_t elem_size) {
  if (new_count <= 0) {
    if (buf) free(buf);
    return NULL;
  }
  return realloc(buf, (size_t)new_count * (size_t)elem_size);
}

void rae_buf_copy(void* src, int64_t src_off, void* dst, int64_t dst_off, int64_t len, int64_t elem_size) {
  if (!src || !dst || len <= 0) return;
  memmove((char*)dst + dst_off * elem_size, (char*)src + src_off * elem_size, (size_t)len * (size_t)elem_size);
}
