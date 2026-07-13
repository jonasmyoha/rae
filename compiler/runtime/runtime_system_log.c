/* Time, bare spawn, ad-hoc JSON bridge, and logging helpers. Mixed kernel and temporary bridge code.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

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

rae_String rae_ext_time_formatTimestamp(int64_t epoch_ms) {
  time_t secs = (time_t)(epoch_ms / 1000);
  struct tm tm_buf;
  struct tm* tm_p = gmtime_r(&secs, &tm_buf);
  if (!tm_p) return (rae_String){NULL, 0, 0, 0};
  char buf[32];
  int n = (int)strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_p);
  if (n <= 0) return (rae_String){NULL, 0, 0, 0};
  uint8_t* data = malloc((size_t)n + 1);
  if (!data) return (rae_String){NULL, 0, 0, 0};
  memcpy(data, buf, (size_t)n);
  data[n] = '\0';
  rae_mem_str_tag(data, n + 1, RAE_SITE_FORMAT_TS);
  return (rae_String){data, (int64_t)n, (int64_t)n + 1, 1};
}

rae_String rae_ext_time_formatDate(int64_t epoch_ms) {
  time_t secs = (time_t)(epoch_ms / 1000);
  struct tm tm_buf;
  struct tm* tm_p = gmtime_r(&secs, &tm_buf);
  if (!tm_p) return (rae_String){NULL, 0, 0, 0};
  char buf[16];
  int n = (int)strftime(buf, sizeof(buf), "%Y-%m-%d", tm_p);
  if (n <= 0) return (rae_String){NULL, 0, 0, 0};
  uint8_t* data = malloc((size_t)n + 1);
  if (!data) return (rae_String){NULL, 0, 0, 0};
  memcpy(data, buf, (size_t)n);
  data[n] = '\0';
  rae_mem_str_tag(data, n + 1, RAE_SITE_FORMAT_DATE);
  return (rae_String){data, (int64_t)n, (int64_t)n + 1, 1};
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

RaeAny rae_ext_json_get(const char* json, const char* field) {
    if (!json || !field) return rae_any_none();
    
    // Tiny naive JSON parser: look for "field": value
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", field);
    const char* key_pos = strstr(json, search);
    if (!key_pos) return rae_any_none();
    
    const char* colon = strchr(key_pos + strlen(search), ':');
    if (!colon) return rae_any_none();
    
    const char* val_start = colon + 1;
    while (*val_start && (*val_start == ' ' || *val_start == '\t' || *val_start == '\n' || *val_start == '\r')) {
        val_start++;
    }
    
    if (*val_start == '\"') {
        // String value
        val_start++;
        const char* val_end = strchr(val_start, '\"');
        if (!val_end) return rae_any_none();
        size_t len = val_end - val_start;
        uint8_t* res = malloc(len + 1);
        memcpy(res, val_start, len);
        res[len] = '\0';
        rae_mem_str_tag(res, (int64_t)len + 1, RAE_SITE_JSON_GET_STR);
        return rae_any_string((rae_String){res, (int64_t)len, (int64_t)len + 1, 1});
    } else if (*val_start == 't') {
        return rae_any_bool(1);
    } else if (*val_start == 'f') {
        return rae_any_bool(0);
    } else if (*val_start == 'n') {
        return rae_any_none();
    } else if (*val_start == '-' || (*val_start >= '0' && *val_start <= '9')) {
        // Number
        char* end;
        double f = strtod(val_start, &end);
        if (strchr(val_start, '.') && strchr(val_start, '.') < end) {
            return rae_any_float(f);
        } else {
            return rae_any_int((int64_t)f);
        }
    } else if (*val_start == '{') {
        // Nested object (simplified: just return the raw string part)
        int depth = 1;
        const char* p = val_start + 1;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        }
        size_t len = p - val_start;
        uint8_t* res = malloc(len + 1);
        memcpy(res, val_start, len);
        res[len] = '\0';
        rae_mem_str_tag(res, (int64_t)len + 1, RAE_SITE_JSON_GET_OBJ);
        return rae_any_string((rae_String){res, (int64_t)len, (int64_t)len + 1, 1}); // We return objects as strings for now
    }
    
    return rae_any_none();
}

void rae_ext_rae_log_any(RaeAny value) {
  if (value.is_view) printf("view ");
  else if (value.is_mod) printf("mod ");
  rae_ext_rae_log_stream_any(value);
  printf("\n");
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_any(RaeAny value) {
  if (value.type == RAE_TYPE_ANY) {
      // If it's a reference to an Any, dereference and recurse
      RaeAny inner = *(RaeAny*)value.as.ptr;
      if (value.is_view) inner.is_view = true;
      if (value.is_mod) inner.is_mod = true;
      rae_ext_rae_log_stream_any(inner);
      return;
  }

  bool is_ref = value.is_view || value.is_mod;

  switch (value.type) {
    case RAE_TYPE_INT64: {
        int64_t v = is_ref ? *(int64_t*)value.as.ptr : value.as.i;
        printf("%lld", (long long)v); 
        break;
    }
    case RAE_TYPE_INT32: {
        int32_t v = is_ref ? *(int32_t*)value.as.ptr : (int32_t)value.as.i;
        printf("%d", v); 
        break;
    }
    case RAE_TYPE_UINT64: {
        uint64_t v = is_ref ? *(uint64_t*)value.as.ptr : (uint64_t)value.as.i;
        printf("%llu", (unsigned long long)v); 
        break;
    }
    case RAE_TYPE_FLOAT64: {
        double v = is_ref ? *(double*)value.as.ptr : value.as.f;
        printf("%g", v); 
        break;
    }
    case RAE_TYPE_FLOAT32: {
        float v = is_ref ? *(float*)value.as.ptr : (float)value.as.f;
        printf("%g", (double)v); 
        break;
    }
    case RAE_TYPE_BOOL: {
        int8_t v = is_ref ? *(int8_t*)value.as.ptr : value.as.b;
        printf("%s", v ? "true" : "false"); 
        break;
    }
    case RAE_TYPE_STRING: {
        rae_String v = is_ref ? *(rae_String*)value.as.ptr : value.as.s;
        if (v.data) fwrite(v.data, 1, v.len, stdout);
        else printf("(null)"); 
        break;
    }
    case RAE_TYPE_CHAR: {
        uint32_t v = is_ref ? *(uint32_t*)value.as.ptr : (uint32_t)value.as.i;
        rae_ext_rae_log_stream_char(v); 
        break;
    }
    case RAE_TYPE_ID: printf("Id(%lld)", (long long)value.as.i); break;
    case RAE_TYPE_KEY: {
        rae_String v = is_ref ? *(rae_String*)value.as.ptr : value.as.s;
        printf("Key(\"");
        if (v.data) fwrite(v.data, 1, v.len, stdout);
        printf("\")");
        break;
    }
    case RAE_TYPE_UINT32: {
        uint32_t v = is_ref ? *(uint32_t*)value.as.ptr : (uint32_t)value.as.i;
        printf("%u", v); 
        break;
    }
    case RAE_TYPE_LIST: printf("[...]"); break;
    case RAE_TYPE_BUFFER: printf("%p", value.as.ptr); break;
    case RAE_TYPE_NONE: printf("none"); break;
    case RAE_TYPE_ANY: printf("Any(...)"); break;
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
    if (i < length) {
      rae_ext_rae_log_stream_any(items[i]);
    } else {
      printf("none");
    }
  }
  printf("), %lld, %lld }", (long long)length, (long long)capacity);
}

void rae_ext_rae_log_stream_list_typed(void* data, int64_t length, int64_t capacity, int elem_kind) {
  printf("{ #(");
  for (int64_t i = 0; i < capacity; i++) {
    if (i > 0) printf(", ");
    if (i >= length) { printf("none"); continue; }
    switch (elem_kind) {
      case RAE_LIST_ELEM_ANY:    rae_ext_rae_log_stream_any(((RaeAny*)data)[i]); break;
      case RAE_LIST_ELEM_INT64:  printf("%lld", (long long)((int64_t*)data)[i]); break;
      case RAE_LIST_ELEM_FLOAT64:printf("%g", ((double*)data)[i]); break;
      case RAE_LIST_ELEM_BOOL:   printf("%s", ((bool*)data)[i] ? "true" : "false"); break;
      case RAE_LIST_ELEM_CHAR32: {
        uint32_t cp = ((uint32_t*)data)[i];
        // Encode as UTF-8 for printable codepoints, else fall back to U+XXXX.
        if (cp < 0x80) { putchar((int)cp); }
        else if (cp < 0x800) { putchar(0xC0 | (cp >> 6)); putchar(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { putchar(0xE0 | (cp >> 12)); putchar(0x80 | ((cp >> 6) & 0x3F)); putchar(0x80 | (cp & 0x3F)); }
        else { putchar(0xF0 | (cp >> 18)); putchar(0x80 | ((cp >> 12) & 0x3F)); putchar(0x80 | ((cp >> 6) & 0x3F)); putchar(0x80 | (cp & 0x3F)); }
        break;
      }
      case RAE_LIST_ELEM_STRING: {
        rae_String s = ((rae_String*)data)[i];
        printf("\"%.*s\"", (int)s.len, (const char*)s.data);
        break;
      }
      default: printf("?"); break;
    }
  }
  printf("), %lld, %lld }", (long long)length, (long long)capacity);
}

void rae_ext_rae_log_list_typed(void* data, int64_t length, int64_t capacity, int elem_kind) {
  rae_ext_rae_log_stream_list_typed(data, length, capacity, elem_kind);
  printf("\n");
  fflush(stdout);
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

void rae_ext_rae_log_string(rae_String value) {
  rae_ext_rae_log_stream_string(value);
  printf("\n");
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_string(rae_String value) {
  if (value.data) {
    fwrite(value.data, 1, value.len, stdout);
  }
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

void rae_ext_rae_log_char(uint32_t value) {
  rae_ext_rae_log_stream_char(value);
  printf("\n");
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_char(uint32_t value) {
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

void rae_ext_rae_log_key(rae_String value) {
  rae_ext_rae_log_stream_string(value);
  printf("\n");
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_key(rae_String value) {
  rae_ext_rae_log_stream_string(value);
}

void rae_ext_rae_log_float(double value) {
  printf("%g\n", value);
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_float(double value) {
  printf("%g", value);
  rae_flush_stdout();
}

