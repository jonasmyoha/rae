/* diag.c - Diagnostic implementation */

#include "diag.h"
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

void diag_ctx_error(DiagState* state, const char* file, int line, int col, const char* message) {
  diag_ctx_report(state, file, line, col, message);
}

void diag_ctx_report(DiagState* state, const char* file, int line, int col, const char* message) {
  state->error_count++;
  fprintf(stderr, "%s:%d:%d: %s\n", simplify_path(file), line, col, message);
  if (file && line > 0) {
    print_source_line(file, line, col);
  }
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

void (diag_fatal)(const char* message) {

  diag_ctx_fatal(&g_diag_legacy, message);
}

void diag_reset(void) {
  diag_ctx_reset(&g_diag_legacy);
}

int diag_error_count(void) {
  return diag_ctx_error_count(&g_diag_legacy);
}
