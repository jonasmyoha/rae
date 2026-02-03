#include "rae_runtime.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

static void rae_flush_stdout(void) {
  fflush(stdout);
}

static int64_t g_tick_counter = 0;

int64_t rae_ext_nextTick(void) {
  return ++g_tick_counter;
}

int64_t rae_ext_nowMs(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

int64_t rae_ext_nowNs(void) {
#ifdef __APPLE__
  static mach_timebase_info_data_t timebase;
  if (timebase.denom == 0) mach_timebase_info(&timebase);
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

void rae_ext_rae_log_any(RaeAny value) {
  rae_ext_rae_log_stream_any(value);
  printf("\n");
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_any(RaeAny value) {
  switch (value.type) {
    case RAE_TYPE_INT: printf("%lld", (long long)value.as.i); break;
    case RAE_TYPE_FLOAT: printf("%g", value.as.f); break;
    case RAE_TYPE_BOOL: printf("%s", value.as.b ? "true" : "false"); break;
    case RAE_TYPE_STRING: printf("%s", value.as.s ? value.as.s : "(null)"); break;
    case RAE_TYPE_CHAR: rae_ext_rae_log_stream_char(value.as.i); break;
    case RAE_TYPE_ID: printf("Id(%lld)", (long long)value.as.i); break;
    case RAE_TYPE_KEY: printf("Key(\"%s\")", value.as.s ? value.as.s : "(null)"); break;
    case RAE_TYPE_LIST: printf("[...]"); break;
    case RAE_TYPE_BUFFER: printf("#(...)"); break;
    case RAE_TYPE_NONE: printf("none"); break;
  }
}

void rae_ext_rae_log_list_fields(RaeAny* items, int64_t length, int64_t capacity) {
  rae_ext_rae_log_stream_list_fields(items, length, capacity);
  printf("\n");
  fflush(stdout);
}

void rae_ext_rae_log_stream_list_fields(RaeAny* items, int64_t length, int64_t capacity) {
  printf("{ #(");
  for (int64_t i = 0; i < capacity; i++) {
    if (i > 0) printf(", ");
    rae_ext_rae_log_stream_any(items[i]);
  }
  printf("), %lld, %lld }", (long long)length, (long long)capacity);
}

void rae_ext_rae_log_cstr(const char* text) {
  if (!text) {
    fputs("(null)\n", stdout);
    rae_flush_stdout();
    return;
  }
  fputs(text, stdout);
  fputc('\n', stdout);
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_cstr(const char* text) {
  if (!text) {
    return;
  }
  fputs(text, stdout);
  rae_flush_stdout();
}

void rae_ext_rae_log_i64(int64_t value) {
  printf("%lld\n", (long long)value);
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_i64(int64_t value) {
  printf("%lld", (long long)value);
  rae_flush_stdout();
}

void rae_ext_rae_log_bool(int8_t value) {
  printf("%s\n", value ? "true" : "false");
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_bool(int8_t value) {
  printf("%s", value ? "true" : "false");
  rae_flush_stdout();
}

void rae_ext_rae_log_char(int64_t value) {
  rae_ext_rae_log_stream_char(value);
  printf("\n");
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_char(int64_t value) {
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

void rae_ext_rae_log_id(int64_t value) {
  printf("%lld\n", (long long)value);
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_id(int64_t value) {
  printf("%lld", (long long)value);
  rae_flush_stdout();
}

void rae_ext_rae_log_key(const char* value) {
  printf("%s\n", value ? value : "(null)");
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_key(const char* value) {
  printf("%s", value ? value : "(null)");
  rae_flush_stdout();
}

void rae_ext_rae_log_float(double value) {
  printf("%g\n", value);
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_float(double value) {
  printf("%g", value);
  rae_flush_stdout();
}

const char* rae_ext_rae_str_concat(const char* a, const char* b) {
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

int64_t rae_ext_rae_str_len(const char* s) {
  if (!s) return 0;
  return (int64_t)strlen(s);
}

int64_t rae_ext_rae_str_compare(const char* a, const char* b) {
  if (!a && !b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return (int64_t)strcmp(a, b);
}

int8_t rae_ext_rae_str_eq(const char* a, const char* b) {
  if (a == b) return 1;
  if (!a || !b) return 0;
  return strcmp(a, b) == 0;
}

int64_t rae_ext_rae_str_hash(const char* s) {
  if (!s) return 0;
  // FNV-1a hash
  uint64_t hash = 0xcbf29ce484222325ULL;
  while (*s) {
    hash ^= (uint64_t)(unsigned char)(*s++);
    hash *= 0x100000001b3ULL;
  }
  return (int64_t)hash;
}

const char* rae_ext_rae_str_sub(const char* s, int64_t start, int64_t len) {
  if (!s) return "";
  int64_t slen = (int64_t)strlen(s);
  if (start < 0) start = 0;
  if (start >= slen) return "";
  if (start + len > slen) len = slen - start;
  if (len <= 0) return "";
  
  char* result = malloc((size_t)len + 1);
  if (result) {
    memcpy(result, s + start, (size_t)len);
    result[len] = '\0';
  }
  return result;
}

int8_t rae_ext_rae_str_contains(const char* s, const char* sub) {
  if (!s || !sub) return 0;
  return strstr(s, sub) != NULL;
}

int8_t rae_ext_rae_str_starts_with(const char* s, const char* prefix) {
  if (!s || !prefix) return 0;
  size_t len_s = strlen(s);
  size_t len_p = strlen(prefix);
  if (len_p > len_s) return 0;
  return strncmp(s, prefix, len_p) == 0;
}

int8_t rae_ext_rae_str_ends_with(const char* s, const char* suffix) {
  if (!s || !suffix) return 0;
  size_t len_s = strlen(s);
  size_t len_suffix = strlen(suffix);
  if (len_suffix > len_s) return 0;
  return strncmp(s + len_s - len_suffix, suffix, len_suffix) == 0;
}

int64_t rae_ext_rae_str_index_of(const char* s, const char* sub) {
  if (!s || !sub) return -1;
  const char* p = strstr(s, sub);
  if (!p) return -1;
  return (int64_t)(p - s);
}

const char* rae_ext_rae_str_trim(const char* s) {
  if (!s) return "";
  while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
  if (!*s) return "";
  const char* end = s + strlen(s) - 1;
  while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
  size_t len = (size_t)(end - s + 1);
  char* result = malloc(len + 1);
  if (result) {
    memcpy(result, s, len);
    result[len] = '\0';
  }
  return result;
}

double rae_ext_rae_str_to_f64(const char* s) {
  if (!s) return 0.0;
  return atof(s);
}

int64_t rae_ext_rae_str_to_i64(const char* s) {
  if (!s) return 0;
  return (int64_t)atoll(s);
}

const char* rae_ext_rae_io_read_line(void) {
  char* buffer = NULL;
  size_t len = 0;
  if (getline(&buffer, &len, stdin) == -1) {
    free(buffer);
    return "";
  }
  // Remove newline
  size_t blen = strlen(buffer);
  if (blen > 0 && buffer[blen-1] == '\n') buffer[blen-1] = '\0';
  return buffer;
}

int64_t rae_ext_rae_io_read_char(void) {
  return (int64_t)getchar();
}

void rae_ext_rae_sys_exit(int64_t code) {
  exit((int)code);
}

const char* rae_ext_rae_sys_get_env(const char* name) {
  if (!name) return NULL;
  return getenv(name);
}

const char* rae_ext_rae_sys_read_file(const char* path) {
  if (!path) return NULL;
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char* buffer = malloc((size_t)len + 1);
  if (buffer) {
    fread(buffer, 1, (size_t)len, f);
    buffer[len] = '\0';
  }
  fclose(f);
  return buffer;
}

int8_t rae_ext_rae_sys_write_file(const char* path, const char* content) {
  if (!path || !content) return 0;
  FILE* f = fopen(path, "wb");
  if (!f) return 0;
  size_t len = strlen(content);
  size_t written = fwrite(content, 1, len, f);
  fclose(f);
  return written == len;
}

const char* rae_ext_rae_str_i64(int64_t v) {
  char* buffer = malloc(32);
  if (buffer) {
    sprintf(buffer, "%lld", (long long)v);
  }
  return buffer;
}

const char* rae_ext_rae_str_i64_ptr(int64_t* v) {
  return rae_ext_rae_str_i64(*v);
}

const char* rae_ext_rae_str_f64(double v) {
  char* buffer = malloc(32);
  if (buffer) {
    sprintf(buffer, "%g", v);
  }
  return buffer;
}

const char* rae_ext_rae_str_f64_ptr(double* v) {
  return rae_ext_rae_str_f64(*v);
}

const char* rae_ext_rae_str_bool(int8_t v) {
  return v ? "true" : "false";
}

const char* rae_ext_rae_str_bool_ptr(int8_t* v) {
  return rae_ext_rae_str_bool(*v);
}

const char* rae_ext_rae_str_char(int64_t v) {
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

const char* rae_ext_rae_str_cstr(const char* s) {
  return s;
}

const char* rae_ext_rae_str_cstr_ptr(const char** s) {
  return *s;
}

static uint64_t g_rae_random_state = 0x123456789ABCDEF0ULL;

void rae_ext_rae_seed(int64_t seed) {
  g_rae_random_state = (uint64_t)seed;
}

static uint32_t rae_next_u32(void) {
  g_rae_random_state = g_rae_random_state * 6364136223846793005ULL + 1;
  return (uint32_t)(g_rae_random_state >> 32);
}

double rae_ext_rae_random(void) {
  return (double)rae_next_u32() / (double)4294967295.0;
}

int64_t rae_ext_rae_random_int(int64_t min, int64_t max) {
  if (min >= max) return min;
  uint64_t range = (uint64_t)(max - min + 1);
  return min + (int64_t)(rae_next_u32() % range);
}

void* rae_ext_rae_buf_alloc(int64_t count, int64_t elem_size) {
  if (count <= 0) return NULL;
  return calloc((size_t)count, (size_t)elem_size);
}

void rae_ext_rae_buf_free(void* buf) {
  if (buf) free(buf);
}

void* rae_ext_rae_buf_resize(void* buf, int64_t new_count, int64_t elem_size) {
  if (new_count <= 0) {
    if (buf) free(buf);
    return NULL;
  }
  return realloc(buf, (size_t)new_count * (size_t)elem_size);
}

void rae_ext_rae_buf_copy(void* src, int64_t src_off, void* dst, int64_t dst_off, int64_t len, int64_t elem_size) {
  if (!src || !dst || len <= 0) return;
  memmove((char*)dst + dst_off * elem_size, (char*)src + src_off * elem_size, (size_t)len * (size_t)elem_size);
}

double rae_ext_rae_int_to_float(int64_t i) {
  return (double)i;
}

#ifdef RAE_HAS_RAYLIB
/* Raylib wrappers for C backend */
#include <raylib.h>

void rae_ext_initWindow(int64_t width, int64_t height, const char* title) {
    InitWindow((int)width, (int)height, title);
}

void rae_ext_setConfigFlags(int64_t flags) {
    SetConfigFlags((unsigned int)flags);
}

void rae_ext_drawCubeWires(Vector3 pos, double width, double height, double length, Color color) {
    DrawCubeWires(pos, (float)width, (float)height, (float)length, color);
}

void rae_ext_drawSphere(Vector3 centerPos, double radius, Color color) {
    DrawSphere(centerPos, (float)radius, color);
}

double rae_ext_getTime(void) {
    return GetTime();
}

Color rae_ext_colorFromHSV(double hue, double saturation, double value) {
    return ColorFromHSV((float)hue, (float)saturation, (float)value);
}
#endif
