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

bool rae_version_exact(const char* text, RaeSemver* out) {
  int dots = 0;
  bool digit_before = false;
  if (!text || !text[0]) return false;
  for (const char* c = text; *c; ++c) {
    if (*c == '.') {
      if (!digit_before) return false;
      dots += 1;
      digit_before = false;
    } else if (*c >= '0' && *c <= '9') {
      digit_before = true;
    } else {
      return false;
    }
  }
  if (dots != 2 || !digit_before) return false;
  return rae_semver_parse(text, out);
}

int rae_semver_compare(RaeSemver left, RaeSemver right) {
  if (left.major != right.major) return left.major - right.major;
  if (left.minor != right.minor) return left.minor - right.minor;
  return left.patch - right.patch;
}
