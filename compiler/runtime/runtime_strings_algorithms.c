/* String algorithms above the allocation kernel. Most ordinary policy is moving
 * to lib/string.rae; these C entry points remain as compatibility bridges until
 * no generated or legacy code path references them.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

rae_String rae_ext_rae_str_concat(rae_String a, rae_String b) {
  int64_t len_a = a.len;
  int64_t len_b = b.len;
  uint8_t* result_data = malloc(len_a + len_b + 1);
  if (result_data) {
    if (a.data) memcpy(result_data, a.data, len_a);
    if (b.data) memcpy(result_data + len_a, b.data, len_b);
    result_data[len_a + len_b] = '\0';
    rae_mem_str_tag(result_data, len_a + len_b + 1, RAE_SITE_CONCAT);
    /* Register with the temp pool, matching the str_interp contract.
     * Callers that bind the result (let / assign / ret) detach via
     * rae_string_pool_take; transient uses (foo(a + b) at expr-stmt
     * scope) get swept by the surrounding mark/flush. Without this
     * line, every concat result whose owner doesn't explicitly call
     * str_free leaks until process exit. */
    rae_string_pool_register(result_data);
  }
  return (rae_String){result_data, len_a + len_b, len_a + len_b + 1, 1};
}

rae_String rae_ext_rae_str_concat_cstr(rae_String a, rae_String b) {
  return rae_ext_rae_str_concat(a, b);
}

int64_t rae_ext_rae_str_len(rae_String s) {
  return s.len;
}

int64_t rae_ext_rae_str_compare(rae_String a, rae_String b) {
  if (a.len < b.len) {
    int res = memcmp(a.data, b.data, a.len);
    return res == 0 ? -1 : res;
  } else if (a.len > b.len) {
    int res = memcmp(a.data, b.data, b.len);
    return res == 0 ? 1 : res;
  } else {
    return memcmp(a.data, b.data, a.len);
  }
}

rae_Bool rae_ext_rae_str_eq(rae_String a, rae_String b) {
  if (a.len != b.len) return false;
  if (a.len == 0) return true;
  return memcmp(a.data, b.data, a.len) == 0;
}

int64_t rae_ext_rae_str_hash(rae_String s) {
  if (!s.data) return 0;
  // FNV-1a hash
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (int64_t i = 0; i < s.len; i++) {
    hash ^= (uint64_t)s.data[i];
    hash *= 0x100000001b3ULL;
  }
  return (int64_t)hash;
}

rae_String rae_ext_rae_str_sub(rae_String s, int64_t start, int64_t len) {
  if (!s.data) return (rae_String){NULL, 0, 0, 0};
  if (start < 0) start = 0;
  if (start >= s.len) return (rae_String){NULL, 0, 0, 0};
  if (start + len > s.len) len = s.len - start;
  if (len <= 0) return (rae_String){NULL, 0, 0, 0};

  uint8_t* result_data = malloc((size_t)len + 1);
  if (result_data) {
    memcpy(result_data, s.data + start, (size_t)len);
    result_data[len] = '\0';
    rae_mem_str_tag(result_data, len + 1, RAE_SITE_SUB);
  }
  return (rae_String){result_data, len, len + 1, 1};
}

rae_Bool rae_ext_rae_str_contains(rae_String s, rae_String sub) {
  if (!s.data || !sub.data) return false;
  if (sub.len == 0) return true;
  if (sub.len > s.len) return false;
  // Naive search because we don't necessarily have NUL termination at the right place if it's a subslice
  // But we DO ensure NUL termination in our helpers.
  return strstr((const char*)s.data, (const char*)sub.data) != NULL;
}

rae_Bool rae_ext_rae_str_starts_with(rae_String s, rae_String prefix) {
  if (prefix.len > s.len) return false;
  if (prefix.len == 0) return true;
  return memcmp(s.data, prefix.data, prefix.len) == 0;
}

rae_Bool rae_ext_rae_str_ends_with(rae_String s, rae_String suffix) {
  if (suffix.len > s.len) return false;
  if (suffix.len == 0) return true;
  return memcmp(s.data + s.len - suffix.len, suffix.data, suffix.len) == 0;
}

int64_t rae_ext_rae_str_index_of(rae_String s, rae_String sub) {
  if (!s.data || !sub.data) return -1;
  if (sub.len == 0) return 0;
  const char* p = strstr((const char*)s.data, (const char*)sub.data);
  if (!p) return -1;
  return (int64_t)(p - (const char*)s.data);
}

rae_String rae_ext_rae_str_to_lower(rae_String s) {
  if (!s.data || s.len == 0) return (rae_String){NULL, 0, 0, 0};
  uint8_t* out = malloc(s.len + 1);
  if (!out) return (rae_String){NULL, 0, 0, 0};
  for (int64_t i = 0; i < s.len; i++) {
    uint8_t c = s.data[i];
    if (c >= 'A' && c <= 'Z') {
      out[i] = c + ('a' - 'A');
    } else {
      out[i] = c;
    }
  }
  out[s.len] = '\0';
  rae_mem_str_tag(out, s.len + 1, RAE_SITE_CONCAT);
  rae_string_pool_register(out);
  return (rae_String){out, s.len, s.len + 1, 1};
}

rae_String rae_ext_rae_str_trim(rae_String s) {
  if (!s.data || s.len == 0) return (rae_String){NULL, 0, 0, 0};
  int64_t start = 0;
  while (start < s.len && (s.data[start] == ' ' || s.data[start] == '\t' || s.data[start] == '\n' || s.data[start] == '\r')) start++;
  if (start == s.len) return (rae_String){NULL, 0, 0, 0};
  int64_t end = s.len - 1;
  while (end > start && (s.data[end] == ' ' || s.data[end] == '\t' || s.data[end] == '\n' || s.data[end] == '\r')) end--;
  return rae_ext_rae_str_sub(s, start, end - start + 1);
}

uint32_t rae_ext_rae_str_at(rae_String s, int64_t index) {
  if (!s.data || index < 0 || index >= s.len) return 0;
  uint8_t c = s.data[index];
  if (c < 0x80) return (uint32_t)c;
  if ((c & 0xE0) == 0xC0) {
    if (index + 1 >= s.len) return (uint32_t)c;
    return (uint32_t)(((c & 0x1F) << 6) | (s.data[index+1] & 0x3F));
  }
  if ((c & 0xF0) == 0xE0) {
    if (index + 2 >= s.len) return (uint32_t)c;
    return (uint32_t)(((c & 0x0F) << 12) | ((s.data[index+1] & 0x3F) << 6) | (s.data[index+2] & 0x3F));
  }
  if ((c & 0xF8) == 0xF0) {
    if (index + 3 >= s.len) return (uint32_t)c;
    return (uint32_t)(((c & 0x07) << 18) | ((s.data[index+1] & 0x3F) << 12) | ((s.data[index+2] & 0x3F) << 6) | (s.data[index+3] & 0x3F));
  }
  return (uint32_t)c;
}

double rae_ext_rae_str_to_f64(rae_String s) {
  if (!s.data) return 0.0;
  return atof((const char*)s.data);
}

int64_t rae_ext_rae_str_to_i64(rae_String s) {
  if (!s.data) return 0;
  return (int64_t)atoll((const char*)s.data);
}
