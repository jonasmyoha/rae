/* diag.c - Diagnostic implementation */

#include "diag.h"
#include "progress.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <unistd.h>

static const char* simplify_path(const char* path) {
  if (!path) return "<unknown>";
  static char cwd[1024];
  if (getcwd(cwd, sizeof(cwd)) == NULL) return path;
  
  size_t cwd_len = strlen(cwd);
  if (strncmp(path, cwd, cwd_len) == 0) {
    const char* rel = path + cwd_len;
    if (*rel == '/' || *rel == '\\') return rel + 1;
    if (*rel == '\0') return ".";
    return rel;
  }
  return path;
}

const char* diag_simplify_path(const char* path) {
  return simplify_path(path);
}

static bool g_diag_quiet = false;

void diag_set_quiet(bool quiet) {
  g_diag_quiet = quiet;
}

static void print_source_line(const char* file, int line, int col) {
  if (!file || line <= 0) return;
  FILE* f = fopen(file, "r");
  if (!f) return;

  char buffer[1024];
  int current_line = 1;
  while (fgets(buffer, sizeof(buffer), f)) {
    if (current_line == line) {
      fprintf(stderr, " %5d | %s", line, buffer);
      
      size_t len = strlen(buffer);
      if (len > 0 && buffer[len-1] != '\n') {
        fputc('\n', stderr);
      }

      fprintf(stderr, "       | ");
      for (int i = 1; i < col; i++) {
        fputc(' ', stderr);
      }
      fprintf(stderr, "^~~~\n");
      break;
    }
    current_line++;
  }
  fclose(f);
}

void diag_init(DiagState* state) {
  state->error_count = 0;
  state->had_fatal = false;
}

/* ---- the instance being analyzed (docs/generic-instances-design.md section 2 point 4) ----
 * Inside an instance of a generic function, a diagnostic names the instance. One at a
 * place that already had the same message is not printed again: every instance of a
 * template would report it. */
enum { context_depth = 16 };
static _Thread_local const char* g_context[context_depth];
static _Thread_local int g_context_count;
static _Thread_local uint64_t* g_seen;
static _Thread_local size_t g_seen_count, g_seen_cap;

void diag_push_context(const char* text) {
  if (g_context_count < context_depth) g_context[g_context_count] = text;
  g_context_count++;
}

void diag_pop_context(void) {
  if (g_context_count > 0) g_context_count--;
}

static uint64_t place_key(const char* file, int line, int col, const char* message) {
  uint64_t hash = 1469598103934665603ull;
  const char* parts[2] = { simplify_path(file), message ? message : "" };
  for (int p = 0; p < 2; p++) {
    for (const char* c = parts[p]; *c; c++) hash = (hash ^ (unsigned char)*c) * 1099511628211ull;
    hash = (hash ^ 0xff) * 1099511628211ull;
  }
  return (hash ^ (uint64_t)line * 31 ^ (uint64_t)col * 1000003) * 1099511628211ull;
}

static bool seen(uint64_t key) {
  for (size_t i = 0; i < g_seen_count; i++) if (g_seen[i] == key) return true;
  return false;
}

static void remember(uint64_t key) {
  if (g_seen_count == g_seen_cap) {
    g_seen_cap = g_seen_cap ? g_seen_cap * 2 : 256;
    g_seen = realloc(g_seen, g_seen_cap * sizeof *g_seen);
  }
  g_seen[g_seen_count++] = key;
}

/* true: print it, the first time its place and text come. A place an instance's
 * error was printed for takes no other message after it (the C backend's own
 * words for the same mistake). */
static bool first_at_place(const char* file, int line, int col, const char* message) {
  uint64_t key = place_key(file, line, col, message);
  uint64_t place = place_key(file, line, col, "\x01instance");
  if (seen(key) || seen(place)) return false;
  if (g_context_count > 0) remember(place);
  remember(key);
  return true;
}

static void print_context(void) {
  if (g_context_count <= 0) return;
  int innermost = g_context_count <= context_depth ? g_context_count - 1 : context_depth - 1;
  fprintf(stderr, " (in %s)", g_context[innermost]);
}

void diag_ctx_error(DiagState* state, const char* file, int line, int col, const char* message) {
  diag_ctx_report(state, file, line, col, message);
}

void diag_ctx_report(DiagState* state, const char* file, int line, int col, const char* message) {
  progress_hide();
  if (g_diag_quiet) { state->error_count++; return; }
  if (!first_at_place(file, line, col, message)) return;
  state->error_count++;
  fprintf(stderr, "%s:%d:%d: %s", simplify_path(file), line, col, message);
  print_context();
  fputc('\n', stderr);
  if (file && line > 0) {
    print_source_line(file, line, col);
  }
  fflush(stderr);
}

// A non-fatal WARNING: prints but does NOT increment error_count, so the build
// still succeeds (used by the no-globals warning, #763).
void diag_ctx_warn(DiagState* state, const char* file, int line, int col, const char* message) {
  progress_hide();
  (void)state;
  if (g_diag_quiet) return;
  if (!first_at_place(file, line, col, message)) return;
  fprintf(stderr, "%s:%d:%d: warning: %s", simplify_path(file), line, col, message);
  print_context();
  fputc('\n', stderr);
  fflush(stderr);
}

int diag_ctx_error_count(DiagState* state) {
  return state->error_count;
}

void diag_ctx_reset(DiagState* state) {
  state->error_count = 0;
  state->had_fatal = false;
}

void diag_ctx_fatal(DiagState* state, const char* message) {
  progress_hide();
  state->had_fatal = true;
  fprintf(stderr, "fatal error: %s\n", message);
  exit(1);
}

// Global state for legacy fallback
static DiagState g_diag_legacy = {0};

void (diag_error)(const char* file, int line, int col, const char* message) {
    diag_ctx_error(&g_diag_legacy, file, line, col, message);
}

void (diag_report)(const char* file, int line, int col, const char* message) {
    diag_ctx_report(&g_diag_legacy, file, line, col, message);
}

void (diag_warn)(const char* file, int line, int col, const char* message) {
    diag_ctx_warn(&g_diag_legacy, file, line, col, message);
}

void (diag_fatal)(const char* message) {

  diag_ctx_fatal(&g_diag_legacy, message);
}

void diag_reset(void) {
  diag_ctx_reset(&g_diag_legacy);
}

int diag_error_count(void) {
  return diag_ctx_error_count(&g_diag_legacy);
}
