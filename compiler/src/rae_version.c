#include "rae_version.h"

#include <ctype.h>
#include <string.h>

/* Read a run of decimal digits into *out, advancing *cursor. Returns false if
 * no digit is present. */
static bool read_uint(const char** cursor, int* out) {
  const char* p = *cursor;
  if (!isdigit((unsigned char)*p)) return false;
  int value = 0;
  while (isdigit((unsigned char)*p)) {
    value = value * 10 + (*p - '0');
    p += 1;
  }
  *cursor = p;
  *out = value;
  return true;
}

bool rae_semver_parse(const char* text, RaeSemver* out) {
  if (!text || !out) return false;
  while (*text == ' ' || *text == '\t') text += 1;
  RaeSemver v = {0, 0, 0};
  if (!read_uint(&text, &v.major)) return false;
  if (*text == '.') {
    text += 1;
    if (!read_uint(&text, &v.minor)) return false;
    if (*text == '.') {
      text += 1;
      if (!read_uint(&text, &v.patch)) return false;
    }
  }
  /* A "-prerelease" or "+build" suffix is accepted and ignored; anything else
   * trailing (after optional spaces) is malformed. */
  if (*text == '-' || *text == '+') {
    *out = v;
    return true;
  }
  while (*text == ' ' || *text == '\t') text += 1;
  if (*text != '\0') return false;
  *out = v;
  return true;
}

static int semver_cmp(RaeSemver a, RaeSemver b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  return 0;
}

typedef struct {
  bool has_low;
  bool low_inclusive;
  RaeSemver low;
  bool has_high;
  bool high_inclusive;
  RaeSemver high;
} VersionRange;

/* Caret upper bound: pre-1.0 the MINOR is breaking, so ^0.3 -> <0.4.0; at
 * >=1.0 the MAJOR is breaking, so ^1.2 -> <2.0.0. */
static RaeSemver caret_upper(RaeSemver low) {
  RaeSemver high = {0, 0, 0};
  if (low.major == 0) {
    high.major = 0;
    high.minor = low.minor + 1;
  } else {
    high.major = low.major + 1;
  }
  return high;
}

/* Parse one "opVERSION" comparator token (e.g. ">=0.3.0") into `range`. */
static bool parse_comparator(const char* token, VersionRange* range) {
  bool is_low;
  bool inclusive;
  if (strncmp(token, ">=", 2) == 0) {
    is_low = true; inclusive = true; token += 2;
  } else if (strncmp(token, "<=", 2) == 0) {
    is_low = false; inclusive = true; token += 2;
  } else if (token[0] == '>') {
    is_low = true; inclusive = false; token += 1;
  } else if (token[0] == '<') {
    is_low = false; inclusive = false; token += 1;
  } else {
    return false;
  }
  RaeSemver v;
  if (!rae_semver_parse(token, &v)) return false;
  if (is_low) {
    range->has_low = true;
    range->low_inclusive = inclusive;
    range->low = v;
  } else {
    range->has_high = true;
    range->high_inclusive = inclusive;
    range->high = v;
  }
  return true;
}

static bool parse_requirement(const char* req, VersionRange* range) {
  memset(range, 0, sizeof(*range));
  if (!req) return false;
  while (*req == ' ' || *req == '\t') req += 1;
  if (*req == '\0') return false;

  if (*req == '>' || *req == '<') {
    /* Explicit range: one or two space-separated comparators. */
    const char* p = req;
    char token[64];
    while (*p) {
      while (*p == ' ' || *p == '\t') p += 1;
      if (*p == '\0') break;
      size_t n = 0;
      while (*p && *p != ' ' && *p != '\t') {
        if (n + 1 >= sizeof(token)) return false;
        token[n++] = *p++;
      }
      token[n] = '\0';
      if (!parse_comparator(token, range)) return false;
    }
    return range->has_low || range->has_high;
  }

  /* Bare version: a caret requirement. */
  RaeSemver low;
  if (!rae_semver_parse(req, &low)) return false;
  range->has_low = true;
  range->low_inclusive = true;
  range->low = low;
  range->has_high = true;
  range->high_inclusive = false;
  range->high = caret_upper(low);
  return true;
}

bool rae_toolchain_satisfies(const char* req, RaeSemver compiler, bool is_dev,
                             bool strict, bool* bad_req) {
  VersionRange range;
  if (!parse_requirement(req, &range)) {
    if (bad_req) *bad_req = true;
    return false;
  }
  if (bad_req) *bad_req = false;

  /* CI-strict runs demand a tagged release; a between-tags build never
   * satisfies a requirement under --check-toolchain. */
  if (strict && is_dev) return false;

  if (range.has_low) {
    int c = semver_cmp(compiler, range.low);
    if (range.low_inclusive ? (c < 0) : (c <= 0)) return false;
  }
  if (range.has_high) {
    int c = semver_cmp(compiler, range.high);
    if (range.high_inclusive ? (c > 0) : (c >= 0)) return false;
  }
  return true;
}
