/* diag.h - Diagnostic and error reporting */

#ifndef DIAG_H
#define DIAG_H

void diag_error(const char* file, int line, int col, const char* message);
void diag_report(const char* file, int line, int col, const char* message);
void diag_fatal(const char* message);
int diag_error_count(void);
void diag_reset(void);

#endif /* DIAG_H */
