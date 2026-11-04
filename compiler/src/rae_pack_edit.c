#include "rae_pack_edit.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Past the string or comment starting at text[at], or `at` itself if neither */
static size_t skip_string_or_comment(const char* text, size_t at) {
  if (text[at] == '"') {
    size_t i = at + 1;
    while (text[i] && text[i] != '"') {
      if (text[i] == '\\' && text[i + 1]) i += 1;
      i += 1;
    }
    return text[i] ? i + 1 : i;
  }
  if (text[at] == '#' || (text[at] == '/' && text[at + 1] == '/')) {
    size_t i = at;
    while (text[i] && text[i] != '\n') i += 1;
    return i;
  }
  return at;
}

/* Where the field `key:` directly inside the pack's block starts, and the end of its
 * value when that is a block. False when the pack has no such field. */
static bool find_top_field(const char* text, const char* key, size_t* start, size_t* end) {
  size_t key_len = strlen(key);
  int depth = 0;
  for (size_t i = 0; text[i];) {
    size_t past = skip_string_or_comment(text, i);
    if (past != i) {
      i = past;
      continue;
    }
    char c = text[i];
    if (c == '{') depth += 1;
    else if (c == '}') depth -= 1;
    bool at_word = depth == 1 && (i == 0 || !(isalnum((unsigned char)text[i - 1]) || text[i - 1] == '_'));
    if (at_word && strncmp(text + i, key, key_len) == 0) {
      size_t after = i + key_len;
      while (text[after] == ' ' || text[after] == '\t') after += 1;
      if (text[after] == ':') {
        size_t open = after + 1;
        while (text[open] && text[open] != '{' && text[open] != '\n') open += 1;
        if (text[open] != '{') return false;
        int inner = 0;
        for (size_t j = open; text[j];) {
          size_t skip = skip_string_or_comment(text, j);
          if (skip != j) {
            j = skip;
            continue;
          }
          if (text[j] == '{') inner += 1;
          else if (text[j] == '}') {
            inner -= 1;
            if (inner == 0) {
              *start = i;
              *end = j + 1;
              return true;
            }
          }
          j += 1;
        }
        return false;
      }
    }
    i += 1;
  }
  return false;
}

static size_t line_start(const char* text, size_t at) {
  while (at > 0 && text[at - 1] != '\n') at -= 1;
  return at;
}

/* The closing brace of the pack's block, at the start of its line */
static bool find_pack_end(const char* text, size_t* at) {
  int depth = 0;
  for (size_t i = 0; text[i];) {
    size_t past = skip_string_or_comment(text, i);
    if (past != i) {
      i = past;
      continue;
    }
    if (text[i] == '{') depth += 1;
    else if (text[i] == '}') {
      depth -= 1;
      if (depth == 0) {
        *at = i;
        return true;
      }
    }
    i += 1;
  }
  return false;
}

static char* splice(const char* text, size_t from, size_t to, const char* middle) {
  size_t total = strlen(text);
  size_t middle_len = strlen(middle);
  char* out = malloc(total - (to - from) + middle_len + 1);
  if (!out) return NULL;
  memcpy(out, text, from);
  memcpy(out + from, middle, middle_len);
  strcpy(out + from + middle_len, text + to);
  return out;
}

char* rae_pack_set_block(const char* text, const char* key, const char* block,
                         const char* const* before_keys) {
  size_t start = 0;
  size_t end = 0;
  if (find_top_field(text, key, &start, &end)) {
    size_t from = line_start(text, start);
    size_t to = end;
    while (text[to] == ' ' || text[to] == '\t') to += 1;
    if (text[to] == '\n') to += 1;
    return splice(text, from, to, block);
  }
  for (const char* const* before = before_keys; before && *before; ++before) {
    if (find_top_field(text, *before, &start, &end)) {
      size_t at = line_start(text, start);
      return splice(text, at, at, block);
    }
  }
  size_t close = 0;
  if (!find_pack_end(text, &close)) return NULL;
  size_t at = line_start(text, close);
  return splice(text, at, at, block);
}

bool rae_pack_has_block(const char* text, const char* key) {
  size_t start = 0;
  size_t end = 0;
  return find_top_field(text, key, &start, &end);
}

/* The string value of `key:` in text[from..to]; "" when it has none */
static void read_string_field(const char* text, size_t from, size_t to, const char* key, char* out, size_t out_len) {
  out[0] = '\0';
  size_t key_len = strlen(key);
  for (size_t i = from; i + key_len < to; ++i) {
    bool at_word = !(isalnum((unsigned char)text[i - 1]) || text[i - 1] == '_');
    if (!at_word || strncmp(text + i, key, key_len) != 0) continue;
    size_t at = i + key_len;
    while (at < to && (text[at] == ' ' || text[at] == '\t')) at += 1;
    if (at >= to || text[at] != ':') continue;
    at += 1;
    while (at < to && (text[at] == ' ' || text[at] == '\t')) at += 1;
    if (at >= to || text[at] != '"') continue;
    at += 1;
    size_t end = at;
    while (end < to && text[end] != '"') end += 1;
    if (end - at < out_len) {
      memcpy(out, text + at, end - at);
      out[end - at] = '\0';
    }
    return;
  }
}

bool rae_pack_read_pin(const char* text, char* version, size_t version_len, char* commit,
                       size_t commit_len, char* check, size_t check_len) {
  version[0] = commit[0] = check[0] = '\0';
  size_t start = 0;
  size_t end = 0;
  if (!find_top_field(text, "rae", &start, &end)) return false;
  read_string_field(text, start, end, "version", version, version_len);
  read_string_field(text, start, end, "commit", commit, commit_len);
  read_string_field(text, start, end, "check", check, check_len);
  return true;
}
