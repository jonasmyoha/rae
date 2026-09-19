/* shader_compose.h - the `shader(files: [...])` compile-time construct.
 *
 * A shader is a declared COMPOSITION of WGSL parts (docs/shaders-and-the-
 * compiler.md): sema resolves each part path at build time, reads it,
 * concatenates the parts in the written order with a `// --- <path>` marker
 * line before each, validates the composed text with the `naga` CLI and
 * embeds the text in the program as a `Shader` value. The runtime never
 * opens a `.wgsl` file for a declared shader. */
#ifndef SHADER_COMPOSE_H
#define SHADER_COMPOSE_H

#include <stdbool.h>
#include <stddef.h>
#include "arena.h"

typedef struct {
  char* text;              /* the composed WGSL, arena-owned */
  size_t text_len;
  const char** resolved;   /* one resolved on-disk path per part, arena-owned */
  int* part_start;         /* composed line (1-based) of each part's first content line */
  size_t part_count;
} ShaderComposition;

/* Resolve every part — next to the declaring `.rae` file first, then against
 * the project root, then the toolchain stdlib for a `lib/...` path (the
 * order the runtime's rae_ext_rae_gb_read_shader used, plus the declaring
 * file's own directory) — read and compose. On a miss, `err` names the part
 * and the paths tried, and the result is false. */
bool shader_compose(Arena* arena,
                    const char* declaring_file,
                    const char* project_root,
                    const char* stdlib_dir,
                    const char** paths,
                    size_t path_count,
                    ShaderComposition* out,
                    char* err,
                    size_t err_cap);

typedef enum {
  SHADER_VALID = 0,        /* naga accepted the text */
  SHADER_INVALID = 1,      /* naga rejected it: `err` holds the message, *part/*line the origin */
  SHADER_NAGA_MISSING = 2, /* no naga executable on this machine */
  SHADER_VALIDATE_OFF = 3  /* RAE_SHADER_VALIDATE=off */
} ShaderValidation;

/* Validate a composition with naga (RAE_NAGA, then PATH, then ~/.cargo/bin).
 * On SHADER_INVALID, `err` is naga's message and *out_part / *out_line the
 * part index and the line WITHIN that part the message points at (-1 when
 * naga gave no location). */
ShaderValidation shader_validate(const ShaderComposition* composition,
                                 const char** part_names,
                                 char* err,
                                 size_t err_cap,
                                 int* out_part,
                                 int* out_line);

/* The naga executable this compiler would run, or NULL. */
const char* shader_naga_path(void);

#endif /* SHADER_COMPOSE_H */
