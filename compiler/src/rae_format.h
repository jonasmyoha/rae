/* rae_format.h - the project formatter (#917).
 *
 * ONE formatter: the in-memory `rae_format_source` renders a Rae module to a
 * canonical buffer and verifies it (the output re-parses, stays under the line
 * cap, and — optionally — dumps the same AST as the input). The CLI, the build
 * preflight (#919), `watch` and the tests all go through it, so there is never
 * a second layout authority. `rae_format_cli` is the `rae format` command:
 * files/dirs formatted in place by default, plus --check / --stdout / --write /
 * --stdin / --json / --rules. */

#ifndef RAE_FORMAT_H
#define RAE_FORMAT_H

#include <stdbool.h>
#include <stddef.h>

/* The 1,000-line file cap (AGENTS.md). A canonical form over this is REFUSED,
 * never written — the module must be split by hand. */
#define RAE_FORMAT_MAX_LINES 1000

typedef struct {
  char* output;       /* malloc'd, NUL-terminated canonical text; NULL when !ok */
  size_t output_len;
  bool ok;            /* formatting AND verification succeeded */
  bool changed;       /* output differs from the input bytes (only meaningful when ok) */
  bool over_cap;      /* canonical form would exceed RAE_FORMAT_MAX_LINES (a kind of !ok) */
  int input_lines;
  int output_lines;
  char message[256];  /* human-readable reason when !ok (parse error / over-cap / verify failure) */
} RaeFormatResult;

/* Format one Rae module IN MEMORY. `path` is used only for diagnostics (and to
 * refuse a `.raepack`, which is path-based — see the CLI); `source`/`len` are
 * the bytes to format. Never touches the filesystem. When `check_ast` is true
 * the input and output ASTs are dumped and compared, a stronger guard that the
 * formatter moved layout and nothing else. Returns `result->ok`; the caller
 * frees with rae_format_result_free. */
bool rae_format_source(const char* path, const char* source, size_t len,
                       bool check_ast, RaeFormatResult* result);

void rae_format_result_free(RaeFormatResult* result);

/* The `rae format` CLI. `argc`/`argv` are the arguments AFTER `format`.
 * Returns the process exit code (0 = success; non-zero = a difference in
 * --check mode, an over-cap refusal, a parse failure, or a write error). */
int rae_format_cli(int argc, char** argv);

#endif /* RAE_FORMAT_H */
