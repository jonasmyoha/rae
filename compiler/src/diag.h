/* diag.h - Diagnostic and error reporting */

#ifndef DIAG_H
#define DIAG_H

#include <stdbool.h>

typedef struct DiagState {
  int error_count;
  bool had_fatal;
} DiagState;

void diag_init(DiagState* state);
void diag_ctx_error(DiagState* state, const char* file, int line, int col, const char* message);
void diag_ctx_report(DiagState* state, const char* file, int line, int col, const char* message);
void diag_ctx_fatal(DiagState* state, const char* message);
int diag_ctx_error_count(DiagState* state);
void diag_ctx_reset(DiagState* state);

// Global fallback for legacy code (will be removed later)
void diag_error(const char* file, int line, int col, const char* message);
void diag_report(const char* file, int line, int col, const char* message);
void diag_fatal(const char* message);
int diag_error_count(void);
void diag_reset(void);

#endif /* DIAG_H */
