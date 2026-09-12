/* rae_format.c - the project formatter entry point + CLI (#917). */

#include "rae_format.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "pretty.h"
#include "pretty_internal.h"  /* PP_MAX_WIDTH / PP_WRAP_ARGS / PP_WRAP_FIELDS for --rules */
#include "raepack.h"
#include "str.h"

/* ------------------------------------------------------------------ */
/* In-memory formatter                                                 */
/* ------------------------------------------------------------------ */

static int count_lines(const char* text, size_t len) {
  int lines = 0;
  for (size_t i = 0; i < len; i++) {
    if (text[i] == '\n') lines++;
  }
  /* A trailing line without a newline still counts. */
  if (len > 0 && text[len - 1] != '\n') lines++;
  return lines;
}

/* Render `module` (already parsed from `source`) to a fresh malloc'd buffer. */
static char* render_module(const AstModule* module, const char* source, size_t* out_len) {
  char* buf = NULL;
  size_t size = 0;
  FILE* mem = open_memstream(&buf, &size);
  if (!mem) return NULL;
  pretty_print_module(module, source, mem);
  fclose(mem);
  if (out_len) *out_len = size;
  return buf;
}

/* Dump `module`'s AST to a fresh malloc'd buffer, for the AST-equivalence
 * check. The dump carries no file path, so input and output are comparable. */
static char* dump_ast(const AstModule* module, size_t* out_len) {
  char* buf = NULL;
  size_t size = 0;
  FILE* mem = open_memstream(&buf, &size);
  if (!mem) return NULL;
  ast_dump_module(module, mem);
  fclose(mem);
  if (out_len) *out_len = size;
  return buf;
}

bool rae_format_source(const char* path, const char* source, size_t len,
                       bool check_ast, RaeFormatResult* result) {
  memset(result, 0, sizeof(*result));
  result->input_lines = count_lines(source, len);

  Arena* arena = arena_create(64 * 1024 * 1024);
  if (!arena) {
    snprintf(result->message, sizeof(result->message), "out of memory");
    return false;
  }

  /* Parse the input. Parse errors are the file's own errors — they print to
   * stderr and the file is left untouched (the CLI counts them per file). */
  diag_reset();
  TokenList tokens = lexer_tokenize(arena, path, source, len, /*strict=*/false);
  AstModule* module = parse_module(arena, path, tokens);
  if (!module || diag_error_count() > 0) {
    snprintf(result->message, sizeof(result->message), "parse error");
    arena_destroy(arena);
    return false;
  }

  size_t out_len = 0;
  char* output = render_module(module, source, &out_len);
  if (!output) {
    snprintf(result->message, sizeof(result->message), "out of memory");
    arena_destroy(arena);
    return false;
  }
  result->output_lines = count_lines(output, out_len);

  /* Over-cap: refuse, never write (AGENTS.md — the module must be split). */
  if (result->output_lines > RAE_FORMAT_MAX_LINES) {
    result->over_cap = true;
    snprintf(result->message, sizeof(result->message),
             "would expand %s from %d to %d lines; split the module",
             path, result->input_lines, result->output_lines);
    free(output);
    arena_destroy(arena);
    return false;
  }

  /* Verify: the output must re-parse cleanly (a formatter bug that produced
   * invalid Rae is caught here, not shipped). */
  Arena* verify_arena = arena_create(64 * 1024 * 1024);
  if (!verify_arena) {
    snprintf(result->message, sizeof(result->message), "out of memory");
    free(output);
    arena_destroy(arena);
    return false;
  }
  diag_reset();
  TokenList out_tokens = lexer_tokenize(verify_arena, path, output, out_len, /*strict=*/false);
  AstModule* out_module = parse_module(verify_arena, path, out_tokens);
  bool verify_ok = out_module && diag_error_count() == 0;

  if (verify_ok && check_ast) {
    size_t a_len = 0, b_len = 0;
    char* a = dump_ast(module, &a_len);
    char* b = dump_ast(out_module, &b_len);
    if (!a || !b || a_len != b_len || memcmp(a, b, a_len) != 0) verify_ok = false;
    free(a);
    free(b);
  }
  diag_reset();
  arena_destroy(verify_arena);
  arena_destroy(arena);

  if (!verify_ok) {
    snprintf(result->message, sizeof(result->message),
             "formatter produced non-equivalent output (internal error)");
    free(output);
    return false;
  }

  result->output = output;
  result->output_len = out_len;
  result->changed = (out_len != len) || memcmp(output, source, len) != 0;
  result->ok = true;
  return true;
}

void rae_format_result_free(RaeFormatResult* result) {
  if (result->output) {
    free(result->output);
    result->output = NULL;
  }
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
  MODE_WRITE,   /* default: rewrite differing files in place */
  MODE_CHECK,   /* list differing files, change nothing, non-zero exit */
  MODE_STDOUT   /* print formatted output to stdout, change nothing */
} FormatMode;

typedef struct {
  FormatMode mode;
  bool json;
  bool stdin_input;
  bool check_ast;
  const char** inputs;
  int input_count;
} FormatCliOptions;

/* Per-file outcome, for the run summary and --json. */
typedef struct {
  char path[PATH_MAX];
  bool changed;
  bool formatted;   /* the bytes on disk were rewritten */
  bool over_cap;
  bool parse_error;
  char message[256];
} FileOutcome;

/* ---- atomic write, permissions preserved, mtime untouched when equal ---- */

static bool write_file_atomic(const char* path, const char* body, size_t len) {
  struct stat st;
  bool have_mode = (stat(path, &st) == 0);

  char tmp[PATH_MAX];
  int written = snprintf(tmp, sizeof(tmp), "%s.raefmt.tmp", path);
  if (written < 0 || (size_t)written >= sizeof(tmp)) return false;

  FILE* f = fopen(tmp, "wb");
  if (!f) return false;
  if (fwrite(body, 1, len, f) != len) {
    fclose(f);
    unlink(tmp);
    return false;
  }
  if (fclose(f) != 0) {
    unlink(tmp);
    return false;
  }
  if (have_mode) chmod(tmp, st.st_mode & 07777);
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    return false;
  }
  return true;
}

/* ---- directory traversal (.rae + .raepack, sorted, no symlinks) ---- */

static bool has_extension(const char* name, const char* ext) {
  size_t nlen = strlen(name), elen = strlen(ext);
  return nlen > elen && strcmp(name + nlen - elen, ext) == 0;
}

static bool is_formattable_file(const char* name) {
  return has_extension(name, ".rae") || has_extension(name, ".raepack");
}

/* Directories that are never source: the git store, build output, and the
 * per-app state folders literally named `.rae` (examples/106_mobile_ui/.rae
 * and the tests/cases per-app state dirs). */
static bool is_skipped_dir(const char* name) {
  return strcmp(name, ".git") == 0 || strcmp(name, ".rae") == 0 ||
         strcmp(name, "build") == 0;
}

typedef struct {
  char** paths;
  int count;
  int capacity;
} PathList;

static void path_list_add(PathList* list, const char* path) {
  if (list->count == list->capacity) {
    list->capacity = list->capacity ? list->capacity * 2 : 64;
    list->paths = realloc(list->paths, (size_t)list->capacity * sizeof(char*));
  }
  list->paths[list->count++] = strdup(path);
}

static int path_cmp(const void* a, const void* b) {
  return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/* Walk `dir` depth-first, collecting formattable files. Entries are sorted at
 * each level so the traversal order is deterministic. Symlinks (file or dir)
 * are not followed. */
static void collect_dir(const char* dir, PathList* out) {
  DIR* handle = opendir(dir);
  if (!handle) {
    fprintf(stderr, "error: could not open directory '%s': %s\n", dir, strerror(errno));
    return;
  }
  PathList entries = {0};
  struct dirent* entry;
  while ((entry = readdir(handle)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    char child[PATH_MAX];
    if ((size_t)snprintf(child, sizeof(child), "%s/%s", dir, entry->d_name) >= sizeof(child)) continue;
    path_list_add(&entries, child);
  }
  closedir(handle);
  qsort(entries.paths, (size_t)entries.count, sizeof(char*), path_cmp);

  for (int i = 0; i < entries.count; i++) {
    const char* child = entries.paths[i];
    const char* base = strrchr(child, '/');
    base = base ? base + 1 : child;
    struct stat st;
    if (lstat(child, &st) != 0) continue;
    if (S_ISLNK(st.st_mode)) continue;  /* follow no symlinks */
    if (S_ISDIR(st.st_mode)) {
      if (!is_skipped_dir(base)) collect_dir(child, out);
    } else if (S_ISREG(st.st_mode) && is_formattable_file(base)) {
      path_list_add(out, child);
    }
    free(entries.paths[i]);
  }
  free(entries.paths);
}

/* ---- JSON helpers ---- */

static void json_string(FILE* out, const char* s) {
  fputc('"', out);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
      case '"': fputs("\\\"", out); break;
      case '\\': fputs("\\\\", out); break;
      case '\n': fputs("\\n", out); break;
      case '\r': fputs("\\r", out); break;
      case '\t': fputs("\\t", out); break;
      default:
        if (c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
  }
  fputc('"', out);
}

/* ---- .raepack (path-based; no in-memory AST) ---- */

static bool format_raepack_to_buffer(const char* path, char** out_body, size_t* out_len,
                                     char* message, size_t message_len) {
  RaePack pack;
  if (!raepack_parse_file(path, &pack, false)) {
    snprintf(message, message_len, "parse error");
    return false;
  }
  char* buf = NULL;
  size_t size = 0;
  FILE* mem = open_memstream(&buf, &size);
  if (!mem) {
    raepack_free(&pack);
    snprintf(message, message_len, "out of memory");
    return false;
  }
  raepack_pretty_print(&pack, mem);
  fclose(mem);
  raepack_free(&pack);
  *out_body = buf;
  *out_len = size;
  return true;
}

/* ---- format one file, applying the mode ---- */

static void format_one_file(const char* path, const FormatCliOptions* options, FileOutcome* outcome) {
  memset(outcome, 0, sizeof(*outcome));
  snprintf(outcome->path, sizeof(outcome->path), "%s", path);

  size_t src_len = 0;
  char* source = read_file(path, &src_len);
  if (!source) {
    outcome->parse_error = true;
    snprintf(outcome->message, sizeof(outcome->message), "could not read file");
    fprintf(stderr, "error: could not read '%s'\n", path);
    return;
  }

  char* body = NULL;
  size_t body_len = 0;
  bool ok;
  if (has_extension(path, ".raepack")) {
    ok = format_raepack_to_buffer(path, &body, &body_len, outcome->message, sizeof(outcome->message));
    if (ok) outcome->changed = (body_len != src_len) || memcmp(body, source, src_len) != 0;
  } else {
    RaeFormatResult result;
    ok = rae_format_source(path, source, src_len, options->check_ast, &result);
    if (ok) {
      body = result.output;   /* take ownership */
      body_len = result.output_len;
      outcome->changed = result.changed;
      result.output = NULL;
    } else {
      outcome->over_cap = result.over_cap;
      snprintf(outcome->message, sizeof(outcome->message), "%s", result.message);
    }
    rae_format_result_free(&result);
  }

  if (!ok) {
    if (!outcome->over_cap) outcome->parse_error = true;
    if (!options->json) {
      if (outcome->over_cap) fprintf(stderr, "error: %s\n", outcome->message);
      else fprintf(stderr, "error: %s: %s\n", path, outcome->message);
    }
    free(source);
    return;
  }

  if (options->mode == MODE_STDOUT) {
    fwrite(body, 1, body_len, stdout);
  } else if (options->mode == MODE_CHECK) {
    if (outcome->changed && !options->json) printf("%s\n", path);
  } else if (options->mode == MODE_WRITE) {
    if (outcome->changed) {
      if (write_file_atomic(path, body, body_len)) {
        outcome->formatted = true;
        if (!options->json) printf("formatted %s\n", path);
      } else {
        outcome->parse_error = true;  /* reuse the error flag: a write failure fails the run */
        snprintf(outcome->message, sizeof(outcome->message), "could not write file");
        if (!options->json) fprintf(stderr, "error: could not write '%s'\n", path);
      }
    }
  }

  free(body);
  free(source);
}

/* ---- --stdin --stdout ---- */

static int format_stdin(const FormatCliOptions* options) {
  size_t cap = 1 << 16, len = 0;
  char* source = malloc(cap);
  if (!source) return 1;
  size_t n;
  while ((n = fread(source + len, 1, cap - len, stdin)) > 0) {
    len += n;
    if (len == cap) {
      cap *= 2;
      char* grown = realloc(source, cap);
      if (!grown) { free(source); return 1; }
      source = grown;
    }
  }
  RaeFormatResult result;
  bool ok = rae_format_source("<stdin>", source, len, options->check_ast, &result);
  int code = 0;
  if (ok) {
    fwrite(result.output, 1, result.output_len, stdout);
  } else {
    fprintf(stderr, "error: <stdin>: %s\n", result.message);
    code = 1;
  }
  rae_format_result_free(&result);
  free(source);
  return code;
}

/* ---- --rules --json ---- */

static void print_rules_json(void) {
  printf("{\n");
  printf("  \"indent\": %d,\n", 2);
  printf("  \"maxWidth\": %d,\n", PP_MAX_WIDTH);
  printf("  \"maxLines\": %d,\n", RAE_FORMAT_MAX_LINES);
  printf("  \"newline\": \"lf\",\n");
  printf("  \"finalNewline\": true,\n");
  printf("  \"wrapParamsFrom\": %d,\n", PP_WRAP_ARGS);
  printf("  \"wrapFieldsFrom\": %d\n", PP_WRAP_FIELDS);
  printf("}\n");
}

/* ---- argument parsing ---- */

static bool parse_cli(int argc, char** argv, FormatCliOptions* options, bool* want_rules) {
  memset(options, 0, sizeof(*options));
  options->mode = MODE_WRITE;
  options->check_ast = true;
  *want_rules = false;
  bool mode_set = false;

  options->inputs = malloc((size_t)(argc > 0 ? argc : 1) * sizeof(char*));

  for (int i = 0; i < argc; i++) {
    const char* arg = argv[i];
    if (strcmp(arg, "--check") == 0) {
      options->mode = MODE_CHECK; mode_set = true;
    } else if (strcmp(arg, "--stdout") == 0) {
      options->mode = MODE_STDOUT; mode_set = true;
    } else if (strcmp(arg, "--write") == 0 || strcmp(arg, "-w") == 0) {
      options->mode = MODE_WRITE; mode_set = true;
    } else if (strcmp(arg, "--stdin") == 0) {
      options->stdin_input = true;
    } else if (strcmp(arg, "--json") == 0) {
      options->json = true;
    } else if (strcmp(arg, "--rules") == 0) {
      *want_rules = true;
    } else if (strcmp(arg, "--no-ast-check") == 0) {
      options->check_ast = false;
    } else if (arg[0] == '-') {
      fprintf(stderr, "error: unknown format option '%s'\n", arg);
      return false;
    } else {
      options->inputs[options->input_count++] = arg;
    }
  }
  (void)mode_set;
  return true;
}

/* ---- legacy single-target aliases (203/204 fixtures) ----
 * `format --output <file> <src>` and `format --write <src>`: the pre-#917
 * spellings that wrote one file. --write is still the default write mode;
 * --output <path> writes the ONE input's canonical form to <path>. */
static int format_legacy_output(const char* src, const char* dest, bool check_ast) {
  size_t src_len = 0;
  char* source = read_file(src, &src_len);
  if (!source) {
    fprintf(stderr, "error: could not read '%s'\n", src);
    return 1;
  }
  RaeFormatResult result;
  int code = 0;
  if (rae_format_source(src, source, src_len, check_ast, &result)) {
    if (!write_file_atomic(dest, result.output, result.output_len)) {
      fprintf(stderr, "error: could not write '%s'\n", dest);
      code = 1;
    }
  } else {
    fprintf(stderr, "error: %s: %s\n", src, result.message);
    code = 1;
  }
  rae_format_result_free(&result);
  free(source);
  return code;
}

int rae_format_cli(int argc, char** argv) {
  /* Legacy `--output <path> <src>`: detect before the general parse so the
   * path argument is not mistaken for an input to format. */
  for (int i = 0; i < argc; i++) {
    if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
      const char* dest = argv[i + 1];
      const char* src = NULL;
      for (int j = 0; j < argc; j++) {
        if (j == i || j == i + 1) continue;
        if (argv[j][0] != '-') { src = argv[j]; break; }
      }
      if (!src) {
        fprintf(stderr, "error: --output requires an input file\n");
        return 1;
      }
      return format_legacy_output(src, dest, true);
    }
  }

  FormatCliOptions options;
  bool want_rules = false;
  if (!parse_cli(argc, argv, &options, &want_rules)) {
    free((void*)options.inputs);
    return 1;
  }

  if (want_rules) {
    print_rules_json();
    free((void*)options.inputs);
    return 0;
  }

  if (options.stdin_input) {
    int code = format_stdin(&options);
    free((void*)options.inputs);
    return code;
  }

  if (options.input_count == 0) {
    fprintf(stderr, "error: format requires at least one file or directory (or --stdin)\n");
    free((void*)options.inputs);
    return 1;
  }

  /* Expand inputs: a directory is walked, a file is taken as-is. */
  PathList files = {0};
  for (int i = 0; i < options.input_count; i++) {
    struct stat st;
    if (stat(options.inputs[i], &st) != 0) {
      fprintf(stderr, "error: '%s' not found\n", options.inputs[i]);
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      collect_dir(options.inputs[i], &files);
    } else {
      path_list_add(&files, options.inputs[i]);
    }
  }

  int changed = 0, over_cap = 0, errors = 0, formatted = 0;
  bool first_json = true;
  if (options.json) printf("[");

  for (int i = 0; i < files.count; i++) {
    FileOutcome outcome;
    format_one_file(files.paths[i], &options, &outcome);
    if (outcome.changed) changed++;
    if (outcome.over_cap) over_cap++;
    if (outcome.parse_error) errors++;
    if (outcome.formatted) formatted++;

    if (options.json) {
      printf("%s\n  {", first_json ? "" : ",");
      first_json = false;
      printf("\"path\": ");
      json_string(stdout, outcome.path);
      printf(", \"changed\": %s", outcome.changed ? "true" : "false");
      printf(", \"formatted\": %s", outcome.formatted ? "true" : "false");
      printf(", \"overCap\": %s", outcome.over_cap ? "true" : "false");
      printf(", \"ok\": %s", (!outcome.parse_error && !outcome.over_cap) ? "true" : "false");
      if (outcome.message[0]) {
        printf(", \"message\": ");
        json_string(stdout, outcome.message);
      }
      printf("}");
    }
  }
  if (options.json) printf("%s]\n", files.count ? "\n" : "");

  for (int i = 0; i < files.count; i++) free(files.paths[i]);
  free(files.paths);
  free((void*)options.inputs);

  /* Exit code: any parse/write error or over-cap refusal fails the run; in
   * --check mode a difference fails too. */
  if (errors > 0 || over_cap > 0) return 1;
  if (options.mode == MODE_CHECK && changed > 0) return 1;
  (void)formatted;
  return 0;
}
