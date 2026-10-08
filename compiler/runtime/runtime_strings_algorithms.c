/* The String operations that stay C (AGENTS.md "Runtime C rule";
 * docs/runtime-c-audit.md row 3): allocating a rae_String (concat, sub), the
 * equality the generated code calls for `is` (a length check and memcmp), and
 * atof for toFloat. Every other String algorithm is Rae in lib/String.rae,
 * over the inline byte primitives in rae_runtime.h.
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

rae_Bool rae_ext_rae_str_eq(rae_String a, rae_String b) {
  if (a.len != b.len) return false;
  if (a.len == 0) return true;
  return memcmp(a.data, b.data, a.len) == 0;
}

/* Byte-indexed substring that never cuts a character (docs/strings.md): a
 * `start` inside a multi-byte UTF-8 sequence moves back to that character's
 * first byte, an end inside one moves forward past its last byte, so the
 * result is whole characters and valid UTF-8. `sub(start: 1, len: 1)` of
 * "héllo" is "é" (two bytes), never a lone 0xC3. Continuation bytes are
 * 0x80..0xBF; a byte that is not one starts a character. */
static int is_utf8_continuation(uint8_t b) { return (b & 0xC0) == 0x80; }

rae_String rae_ext_rae_str_sub(rae_String s, int64_t start, int64_t len) {
  if (!s.data) return (rae_String){NULL, 0, 0, 0};
  if (start < 0) start = 0;
  if (start >= s.len) return (rae_String){NULL, 0, 0, 0};
  if (len <= 0) return (rae_String){NULL, 0, 0, 0};
  int64_t end = (len > s.len - start) ? s.len : start + len;
  while (start > 0 && is_utf8_continuation(s.data[start])) start--;
  while (end < s.len && is_utf8_continuation(s.data[end])) end++;
  len = end - start;
  if (len <= 0) return (rae_String){NULL, 0, 0, 0};

  uint8_t* result_data = malloc((size_t)len + 1);
  if (result_data) {
    memcpy(result_data, s.data + start, (size_t)len);
    result_data[len] = '\0';
    rae_mem_str_tag(result_data, len + 1, RAE_SITE_SUB);
  }
  return (rae_String){result_data, len, len + 1, 1};
}

double rae_ext_rae_str_to_f64(rae_String s) {
  if (!s.data) return 0.0;
  return atof((const char*)s.data);
}

