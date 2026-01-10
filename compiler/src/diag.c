/* diag.c - Diagnostic implementation */

#include "diag.h"
#include <stdio.h>
#include <stdlib.h>

#include <string.h>

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

void diag_error(const char* file, int line, int col, const char* message) {
  diag_report(file, line, col, message);
  exit(1);
}

void diag_report(const char* file, int line, int col, const char* message) {
  fprintf(stderr, "%s:%d:%d: %s\n", file ? file : "<unknown>", line, col, message);
  if (file && line > 0) {
    print_source_line(file, line, col);
  }
}

void diag_fatal(const char* message) {
  fprintf(stderr, "error: %s\n", message);
  exit(1);
}
