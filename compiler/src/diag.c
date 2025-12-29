/* diag.c - Diagnostic implementation */

#include "diag.h"
#include <stdio.h>
#include <stdlib.h>

void diag_error(const char* file, int line, int col, const char* message) {
  fprintf(stderr, "%s:%d:%d: %s\n", file, line, col, message);
  exit(1);
}

void diag_fatal(const char* message) {
  fprintf(stderr, "error: %s\n", message);
  exit(1);
}
