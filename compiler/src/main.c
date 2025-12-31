/* main.c - Rae compiler entry point */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include "arena.h"
#include "str.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "pretty.h"
#include "c_backend.h"
#include "vm.h"
#include "vm_compiler.h"
#include "vm_registry.h"

typedef struct {
  const char* input_path;
  const char* output_path;
  bool write_in_place;
} FormatOptions;

typedef struct {
  const char* input_path;
  bool watch;
} RunOptions;

typedef struct {
  const char* entry_path;
  const char* out_path;
  bool emit_c;
} BuildOptions;

typedef struct ModuleNode {
  char* module_path;
  char* file_path;
  AstModule* module;
  struct ModuleNode* next;
} ModuleNode;

typedef struct {
  ModuleNode* head;
  ModuleNode* tail;
  char* root_path;
  Arena* arena;
} ModuleGraph;

typedef struct ModuleStack {
  const char* module_path;
  struct ModuleStack* next;
} ModuleStack;

static void module_stack_print_chain(const ModuleStack* stack) {
  if (!stack) return;
  module_stack_print_chain(stack->next);
  if (stack->module_path) {
    fprintf(stderr, "    -> %s\n", stack->module_path);
  }
}

static void module_stack_print_trace(const ModuleStack* stack, const char* leaf_module) {
  if (!stack && !leaf_module) {
    return;
  }
  fprintf(stderr, "  import trace:\n");
  module_stack_print_chain(stack);
  if (leaf_module) {
    fprintf(stderr, "    -> %s\n", leaf_module);
  }
}

typedef struct {
  char** files;
  size_t file_count;
  size_t file_capacity;
  char** dirs;
  size_t dir_count;
  size_t dir_capacity;
} WatchSources;

static uint64_t hash_bytes(const char* data, size_t length);
static void watch_sources_init(WatchSources* sources);
static void watch_sources_clear(WatchSources* sources);
static void watch_sources_move(WatchSources* dest, WatchSources* src);
static bool watch_sources_add_file(WatchSources* sources, const char* path);
static bool module_graph_collect_watch_sources(const ModuleGraph* graph, WatchSources* sources);
static bool compile_file_chunk(const char* file_path,
                               Chunk* chunk,
                               uint64_t* out_hash,
                               WatchSources* watch_sources);
static bool build_c_backend_output(const char* entry_file, const char* out_path);
typedef struct {
  WatchSources sources;
  time_t* file_mtimes;
  time_t* dir_mtimes;
  const char* fallback_path;
  time_t fallback_mtime;
} WatchState;
static void watch_state_init(WatchState* state, const char* fallback_path);
static void watch_state_free(WatchState* state);
static bool watch_state_apply_sources(WatchState* state, WatchSources* new_sources);
static const char* watch_state_wait_for_change(WatchState* state);
static int run_vm_file(const char* file_path);
static int run_vm_watch(const char* file_path);

static void format_options_init(FormatOptions* opts) {
  opts->input_path = NULL;
  opts->output_path = NULL;
  opts->write_in_place = false;
}

static bool parse_format_args(int argc, char** argv, FormatOptions* opts) {
  format_options_init(opts);
  int i = 0;
  while (i < argc) {
    const char* arg = argv[i];
    if (strcmp(arg, "--write") == 0 || strcmp(arg, "-w") == 0) {
      opts->write_in_place = true;
      i += 1;
    } else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: %s expects a file path\n", arg);
        return false;
      }
      opts->output_path = argv[i + 1];
      i += 2;
    } else if (arg[0] == '-') {
      fprintf(stderr, "error: unknown format option '%s'\n", arg);
      return false;
    } else {
      if (opts->input_path) {
        fprintf(stderr, "error: multiple input files provided ('%s' and '%s')\n", opts->input_path, arg);
        return false;
      }
      opts->input_path = arg;
      i += 1;
    }
  }
  if (!opts->input_path) {
    fprintf(stderr, "error: format command requires a file argument\n");
    return false;
  }
  if (opts->write_in_place && opts->output_path) {
    fprintf(stderr, "error: --write and --output cannot be used together\n");
    return false;
  }
  return true;
}

static bool parse_run_args(int argc, char** argv, RunOptions* opts) {
  opts->watch = false;
  opts->input_path = NULL;

  int i = 0;
  while (i < argc) {
    const char* arg = argv[i];
    if (strcmp(arg, "--watch") == 0 || strcmp(arg, "-w") == 0) {
      opts->watch = true;
      i += 1;
      continue;
    }
    if (arg[0] == '-') {
      fprintf(stderr, "error: unknown run option '%s'\n", arg);
      return false;
    }
    if (opts->input_path) {
      fprintf(stderr, "error: multiple input files provided ('%s' and '%s')\n", opts->input_path, arg);
      return false;
    }
    opts->input_path = arg;
    i += 1;
  }

  if (!opts->input_path) {
    fprintf(stderr, "error: run command requires a file argument\n");
    return false;
  }
  return true;
}

static bool parse_build_args(int argc, char** argv, BuildOptions* opts) {
  opts->entry_path = NULL;
  opts->out_path = "build/out.c";
  opts->emit_c = false;

  int i = 0;
  while (i < argc) {
    const char* arg = argv[i];
    if (strcmp(arg, "--emit-c") == 0) {
      opts->emit_c = true;
      i += 1;
      continue;
    }
    if (strcmp(arg, "--out") == 0 || strcmp(arg, "--output") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: %s expects a file path\n", arg);
        return false;
      }
      opts->out_path = argv[i + 1];
      i += 2;
      continue;
    }
    if (arg[0] == '-') {
      fprintf(stderr, "error: unknown build option '%s'\n", arg);
      return false;
    }
    if (opts->entry_path) {
      fprintf(stderr, "error: multiple entry files provided ('%s' and '%s')\n", opts->entry_path, arg);
      return false;
    }
    opts->entry_path = arg;
    i += 1;
  }

  if (!opts->entry_path) {
    fprintf(stderr, "error: build command requires an entry file argument\n");
    return false;
  }
  return true;
}

static bool file_exists(const char* path) {
  if (!path) return false;
  struct stat st;
  return stat(path, &st) == 0;
}

static bool module_graph_has_module(const ModuleGraph* graph, const char* module_path) {
  for (ModuleNode* node = graph->head; node; node = node->next) {
    if (strcmp(node->module_path, module_path) == 0) {
      return true;
    }
  }
  return false;
}

static bool module_graph_append(ModuleGraph* graph,
                                const char* module_path,
                                const char* file_path,
                                AstModule* module) {
  ModuleNode* node = calloc(1, sizeof(ModuleNode));
  if (!node) {
    fprintf(stderr, "error: out of memory while building module graph\n");
    return false;
  }
  node->module_path = strdup(module_path);
  node->file_path = strdup(file_path);
  node->module = module;
  if (!node->module_path || !node->file_path) {
    fprintf(stderr, "error: out of memory while duplicating module paths\n");
    free(node->module_path);
    free(node->file_path);
    // Temporarily retain source_text buffers (freed with arena lifetime).
    free(node);
    return false;
  }
  if (!graph->head) {
    graph->head = graph->tail = node;
  } else {
    graph->tail->next = node;
    graph->tail = node;
  }
  return true;
}

static ModuleNode* module_graph_find(const ModuleGraph* graph, const char* module_path) {
  for (ModuleNode* node = graph->head; node; node = node->next) {
    if (strcmp(node->module_path, module_path) == 0) {
      return node;
    }
  }
  return NULL;
}

static bool module_stack_contains(const ModuleStack* stack, const char* module_path) {
  while (stack) {
    if (strcmp(stack->module_path, module_path) == 0) {
      return true;
    }
    stack = stack->next;
  }
  return false;
}

typedef struct {
  char** items;
  size_t count;
  size_t capacity;
} SegmentBuffer;

static void segment_buffer_free(SegmentBuffer* buf) {
  for (size_t i = 0; i < buf->count; ++i) {
    free(buf->items[i]);
  }
  free(buf->items);
  buf->items = NULL;
  buf->count = buf->capacity = 0;
}

static bool segment_buffer_push_copy(SegmentBuffer* buf, const char* start, size_t len) {
  if (len == 0) return true;
  if (buf->count == buf->capacity) {
    size_t new_capacity = buf->capacity ? buf->capacity * 2 : 8;
    char** new_items = realloc(buf->items, new_capacity * sizeof(char*));
    if (!new_items) {
      return false;
    }
    buf->items = new_items;
    buf->capacity = new_capacity;
  }
  char* copy = malloc(len + 1);
  if (!copy) {
    return false;
  }
  memcpy(copy, start, len);
  copy[len] = '\0';
  buf->items[buf->count++] = copy;
  return true;
}

static bool segment_buffer_pop(SegmentBuffer* buf) {
  if (buf->count == 0) {
    return false;
  }
  free(buf->items[buf->count - 1]);
  buf->count -= 1;
  return true;
}

static char* segment_buffer_join(const SegmentBuffer* buf) {
  if (buf->count == 0) return NULL;
  size_t total = 0;
  for (size_t i = 0; i < buf->count; ++i) {
    total += strlen(buf->items[i]);
  }
  total += buf->count - 1;
  char* result = malloc(total + 1);
  if (!result) {
    return NULL;
  }
  size_t offset = 0;
  for (size_t i = 0; i < buf->count; ++i) {
    size_t len = strlen(buf->items[i]);
    memcpy(result + offset, buf->items[i], len);
    offset += len;
    if (i + 1 < buf->count) {
      result[offset++] = '/';
    }
  }
  result[offset] = '\0';
  return result;
}

static bool segment_buffer_append_path(SegmentBuffer* buf, const char* path, bool include_last) {
  if (!path || !*path) return true;
  size_t len = strlen(path);
  size_t limit = len;
  if (!include_last) {
    const char* last = strrchr(path, '/');
    if (!last) {
      return true;
    }
    limit = (size_t)(last - path);
  }
  size_t i = 0;
  while (i < limit) {
    while (i < limit && path[i] == '/') i++;
    size_t start = i;
    while (i < limit && path[i] != '/') i++;
    size_t part_len = i - start;
    if (part_len > 0) {
      if (!segment_buffer_push_copy(buf, path + start, part_len)) {
        return false;
      }
    }
  }
  return true;
}

static char* sanitize_import_spec(const char* spec) {
  size_t len = strlen(spec);
  size_t start = 0;
  while (start < len && isspace((unsigned char)spec[start])) {
    start += 1;
  }
  size_t end = len;
  while (end > start && isspace((unsigned char)spec[end - 1])) {
    end -= 1;
  }
  size_t out_len = end - start;
  char* copy = malloc(out_len + 1);
  if (!copy) {
    return NULL;
  }
  for (size_t i = 0; i < out_len; ++i) {
    char c = spec[start + i];
    if (c == '\\') c = '/';
    copy[i] = c;
  }
  copy[out_len] = '\0';
  return copy;
}

static bool is_relative_spec(const char* spec) {
  if (!spec || !spec[0]) return false;
  if (spec[0] == '/') return false;
  if (spec[0] == '.') return true;
  return strncmp(spec, "./", 2) == 0 || strncmp(spec, "../", 3) == 0;
}

static char* normalize_import_path(const char* current_module_path, const char* spec) {
  char* sanitized = sanitize_import_spec(spec);
  if (!sanitized) {
    fprintf(stderr, "error: out of memory while normalizing module path\n");
    return NULL;
  }
  if (sanitized[0] == '\0') {
    fprintf(stderr, "error: empty module path is not allowed\n");
    free(sanitized);
    return NULL;
  }

  SegmentBuffer segments = {0};
  const char* cursor = sanitized;
  bool treat_as_relative = false;
  if (cursor[0] == '/') {
    while (*cursor == '/') cursor++;
  } else {
    treat_as_relative = is_relative_spec(cursor);
  }

  if (treat_as_relative) {
    if (!current_module_path) {
      fprintf(stderr, "error: relative import '%s' is invalid here\n", spec);
      segment_buffer_free(&segments);
      free(sanitized);
      return NULL;
    }
    if (!segment_buffer_append_path(&segments, current_module_path, false)) {
      segment_buffer_free(&segments);
      free(sanitized);
      fprintf(stderr, "error: out of memory while normalizing module path\n");
      return NULL;
    }
  }

  while (*cursor) {
    while (*cursor == '/') cursor++;
    if (!*cursor) break;
    const char* start = cursor;
    while (*cursor && *cursor != '/') cursor++;
    size_t part_len = cursor - start;
    if (part_len == 0 || (part_len == 1 && start[0] == '.')) {
      continue;
    }
    if (part_len == 2 && start[0] == '.' && start[1] == '.') {
      if (!segment_buffer_pop(&segments)) {
        fprintf(stderr, "error: module path '%s' escapes project root\n", spec);
        segment_buffer_free(&segments);
        free(sanitized);
        return NULL;
      }
      continue;
    }
    if (!segment_buffer_push_copy(&segments, start, part_len)) {
      segment_buffer_free(&segments);
      free(sanitized);
      fprintf(stderr, "error: out of memory while normalizing module path\n");
      return NULL;
    }
  }

  if (segments.count == 0) {
    fprintf(stderr, "error: module path '%s' resolves to nothing\n", spec);
    segment_buffer_free(&segments);
    free(sanitized);
    return NULL;
  }

  char* last = segments.items[segments.count - 1];
  size_t last_len = strlen(last);
  if (last_len > 4 && strcmp(last + last_len - 4, ".rae") == 0) {
    last[last_len - 4] = '\0';
    last_len -= 4;
    if (last_len == 0) {
      fprintf(stderr, "error: module path '%s' is invalid\n", spec);
      segment_buffer_free(&segments);
      free(sanitized);
      return NULL;
    }
  }

  char* joined = segment_buffer_join(&segments);
  if (!joined) {
    fprintf(stderr, "error: out of memory while normalizing module path\n");
  }
  segment_buffer_free(&segments);
  free(sanitized);
  return joined;
}

static char* resolve_module_file(const char* root, const char* module_path) {
  size_t root_len = strlen(root);
  size_t mod_len = strlen(module_path);
  size_t total = root_len + mod_len + 6;
  char* buffer = malloc(total);
  if (!buffer) {
    return NULL;
  }
  if (root_len > 0 && (root[root_len - 1] == '/' || root[root_len - 1] == '\\')) {
    root_len -= 1;
  }
  snprintf(buffer, total, "%.*s/%s.rae", (int)root_len, root, module_path);
  return buffer;
}

static char* derive_module_path(const char* root, const char* file_path) {
  size_t root_len = strlen(root);
  if (root_len > 0 && (root[root_len - 1] == '/' || root[root_len - 1] == '\\')) {
    root_len -= 1;
  }
  if (strncmp(file_path, root, root_len) != 0) {
    fprintf(stderr, "error: file '%s' is outside project root '%s'\n", file_path, root);
    return NULL;
  }
  const char* relative = file_path + root_len;
  if (*relative == '/' || *relative == '\\') {
    relative += 1;
  }
  if (*relative == '\0') {
    fprintf(stderr, "error: unable to derive module path for '%s'\n", file_path);
    return NULL;
  }
  char* normalized = normalize_import_path(NULL, relative);
  return normalized;
}


static bool module_graph_init(ModuleGraph* graph, Arena* arena) {
  memset(graph, 0, sizeof(*graph));
  graph->arena = arena;
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) {
    perror("getcwd");
    return false;
  }
  char* resolved = realpath(cwd, NULL);
  graph->root_path = resolved ? resolved : strdup(cwd);
  if (!graph->root_path) {
    fprintf(stderr, "error: failed to allocate project root path\n");
    return false;
  }
  size_t len = strlen(graph->root_path);
  while (len > 1 && (graph->root_path[len - 1] == '/' || graph->root_path[len - 1] == '\\')) {
    graph->root_path[len - 1] = '\0';
    len -= 1;
  }
  const char* last_sep = strrchr(graph->root_path, '/');
#ifdef _WIN32
  const char* last_sep_win = strrchr(graph->root_path, '\\');
  if (!last_sep || (last_sep_win && last_sep_win > last_sep)) {
    last_sep = last_sep_win;
  }
#endif
  if (last_sep) {
    const char* last_component = last_sep + 1;
    if (strcmp(last_component, "compiler") == 0 && last_sep != graph->root_path) {
      graph->root_path[last_sep - graph->root_path] = '\0';
    }
  }
  return true;
}

static void module_graph_free(ModuleGraph* graph) {
  ModuleNode* node = graph->head;
  while (node) {
    ModuleNode* next = node->next;
    free(node->module_path);
    free(node->file_path);
    free(node);
    node = next;
  }
  graph->head = graph->tail = NULL;
  free(graph->root_path);
  graph->root_path = NULL;
}

static void watch_sources_init(WatchSources* sources) {
  sources->files = NULL;
  sources->file_count = 0;
  sources->file_capacity = 0;
  sources->dirs = NULL;
  sources->dir_count = 0;
  sources->dir_capacity = 0;
}

static void watch_sources_clear(WatchSources* sources) {
  if (!sources) return;
  for (size_t i = 0; i < sources->file_count; ++i) {
    free(sources->files[i]);
  }
  for (size_t i = 0; i < sources->dir_count; ++i) {
    free(sources->dirs[i]);
  }
  free(sources->files);
  free(sources->dirs);
  watch_sources_init(sources);
}

static void watch_sources_move(WatchSources* dest, WatchSources* src) {
  watch_sources_clear(dest);
  *dest = *src;
  watch_sources_init(src);
}

static bool string_list_contains(char** list, size_t count, const char* path) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(list[i], path) == 0) {
      return true;
    }
  }
  return false;
}

static bool watch_sources_ensure_capacity(char*** list, size_t* capacity, size_t required) {
  if (required <= *capacity) {
    return true;
  }
  size_t new_cap = *capacity == 0 ? 8 : (*capacity * 2);
  while (new_cap < required) {
    new_cap *= 2;
  }
  char** resized = realloc(*list, new_cap * sizeof(char*));
  if (!resized) {
    return false;
  }
  *list = resized;
  *capacity = new_cap;
  return true;
}

static bool watch_sources_add_dir(WatchSources* sources, const char* dir_path) {
  if (string_list_contains(sources->dirs, sources->dir_count, dir_path)) {
    return true;
  }
  if (!watch_sources_ensure_capacity(&sources->dirs, &sources->dir_capacity,
                                     sources->dir_count + 1)) {
    return false;
  }
  sources->dirs[sources->dir_count] = strdup(dir_path);
  if (!sources->dirs[sources->dir_count]) {
    return false;
  }
  sources->dir_count += 1;
  return true;
}

static bool watch_sources_add_file(WatchSources* sources, const char* path) {
  if (string_list_contains(sources->files, sources->file_count, path)) {
    return true;
  }
  if (!watch_sources_ensure_capacity(&sources->files, &sources->file_capacity,
                                     sources->file_count + 1)) {
    return false;
  }
  sources->files[sources->file_count] = strdup(path);
  if (!sources->files[sources->file_count]) {
    return false;
  }
  sources->file_count += 1;

  char dir_buffer[PATH_MAX];
  strncpy(dir_buffer, path, sizeof(dir_buffer) - 1);
  dir_buffer[sizeof(dir_buffer) - 1] = '\0';
  char* slash = strrchr(dir_buffer, '/');
#ifdef _WIN32
  char* win_slash = strrchr(dir_buffer, '\\');
  if (!slash || (win_slash && win_slash > slash)) {
    slash = win_slash;
  }
#endif
  if (slash) {
    if (slash == dir_buffer) {
      slash[1] = '\0';
    } else {
      *slash = '\0';
    }
  } else {
    strcpy(dir_buffer, ".");
  }
  return watch_sources_add_dir(sources, dir_buffer);
}

static bool module_graph_collect_watch_sources(const ModuleGraph* graph, WatchSources* sources) {
  for (ModuleNode* node = graph->head; node; node = node->next) {
    if (!watch_sources_add_file(sources, node->file_path)) {
      return false;
    }
  }
  return true;
}

static bool module_graph_load_module(ModuleGraph* graph,
                                     const char* module_path,
                                     const char* file_path,
                                     ModuleStack* stack,
                                     uint64_t* hash_out) {
  if (module_graph_has_module(graph, module_path)) {
    return true;
  }
  if (module_stack_contains(stack, module_path)) {
    fprintf(stderr, "error: cyclic import detected involving '%s'\n", module_path);
    module_stack_print_trace(stack, module_path);
    return false;
  }
  size_t file_size = 0;
  char* source = read_file(file_path, &file_size);
  if (!source) {
    fprintf(stderr, "error: could not read module file '%s'\n", file_path);
    module_stack_print_trace(stack, module_path);
    return false;
  }
  if (hash_out) {
    uint64_t module_hash = hash_bytes(source, file_size);
    *hash_out ^= module_hash + 0x9e3779b97f4a7c15ull + (*hash_out << 6) + (*hash_out >> 2);
  }
  TokenList tokens = lexer_tokenize(graph->arena, file_path, source, file_size);
  AstModule* module = parse_module(graph->arena, file_path, tokens);
  if (!module) {
    free(source);
    return false;
  }
  ModuleStack frame = {.module_path = module_path, .next = stack};
  for (AstImport* import = module->imports; import; import = import->next) {
    char* raw = str_to_cstr(import->module_path);
    if (!raw) {
      fprintf(stderr, "error: out of memory while reading import path\n");
      return false;
    }
    char* normalized = normalize_import_path(module_path, raw);
    free(raw);
    if (!normalized) {
      return false;
    }
    char* child_file = resolve_module_file(graph->root_path, normalized);
    if (!child_file || !file_exists(child_file)) {
      fprintf(stderr, "error: imported module '%s' not found (required by '%s')\n", normalized,
              module_path ? module_path : "<entry>");
      module_stack_print_trace(&frame, normalized);
      free(normalized);
      free(child_file);
      return false;
    }
    if (!module_graph_load_module(graph, normalized, child_file, &frame, hash_out)) {
      free(normalized);
      free(child_file);
      return false;
    }
    free(normalized);
    free(child_file);
  }
  if (!module_graph_append(graph, module_path, file_path, module)) {
    free(source);
    return false;
  }
  free(source);
  return true;
}

static bool scan_directory_for_modules(ModuleGraph* graph,
                                       const char* dir_path,
                                       const char* skip_file,
                                       uint64_t* hash_out) {
  DIR* dir = opendir(dir_path);
  if (!dir) {
    return true;
  }
  struct dirent* entry;
  while ((entry = readdir(dir))) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char child_path[PATH_MAX];
    int written = snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, entry->d_name);
    if (written <= 0 || (size_t)written >= sizeof(child_path)) {
      continue;
    }
    struct stat st;
    if (stat(child_path, &st) != 0) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if (!scan_directory_for_modules(graph, child_path, skip_file, hash_out)) {
        closedir(dir);
        return false;
      }
      continue;
    }
    if (!S_ISREG(st.st_mode)) {
      continue;
    }
    size_t len = strlen(child_path);
    if (len < 4 || strcmp(child_path + len - 4, ".rae") != 0) {
      continue;
    }
    if (skip_file && strcmp(child_path, skip_file) == 0) {
      continue;
    }
    char* module_path = derive_module_path(graph->root_path, child_path);
    if (!module_path) {
      closedir(dir);
      return false;
    }
    if (!module_graph_has_module(graph, module_path)) {
      if (!module_graph_load_module(graph, module_path, child_path, NULL, hash_out)) {
        free(module_path);
        closedir(dir);
        return false;
      }
    }
    free(module_path);
  }
  closedir(dir);
  return true;
}

static bool auto_import_directory(ModuleGraph* graph, const char* entry_file_path, uint64_t* hash_out) {
  char* dir_copy = strdup(entry_file_path);
  if (!dir_copy) {
    fprintf(stderr, "error: out of memory while scanning project directory\n");
    return false;
  }
  char* slash = strrchr(dir_copy, '/');
  if (!slash) {
    free(dir_copy);
    return true;
  }
  *slash = '\0';
  bool ok = scan_directory_for_modules(graph, dir_copy, entry_file_path, hash_out);
  free(dir_copy);
  return ok;
}

static bool directory_has_raepack(const char* dir_path) {
  DIR* dir = opendir(dir_path);
  if (!dir) return false;
  struct dirent* entry;
  while ((entry = readdir(dir))) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    char file_path[PATH_MAX];
    int written = snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, entry->d_name);
    if (written <= 0 || (size_t)written >= sizeof(file_path)) continue;
    struct stat st;
    if (stat(file_path, &st) != 0) continue;
    if (!S_ISREG(st.st_mode)) continue;
    size_t len = strlen(entry->d_name);
    if (len > 8 && strcmp(entry->d_name + len - 8, ".raepack") == 0) {
      closedir(dir);
      return true;
    }
  }
  closedir(dir);
  return false;
}

static bool directory_has_only_entry_file(const char* dir_path, const char* entry_file_path) {
  DIR* dir = opendir(dir_path);
  if (!dir) return false;
  size_t rae_count = 0;
  struct dirent* entry;
  while ((entry = readdir(dir))) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    char file_path[PATH_MAX];
    int written = snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, entry->d_name);
    if (written <= 0 || (size_t)written >= sizeof(file_path)) continue;
    struct stat st;
    if (stat(file_path, &st) != 0) continue;
    if (!S_ISREG(st.st_mode)) continue;
    size_t len = strlen(file_path);
    if (len < 4 || strcmp(file_path + len - 4, ".rae") != 0) continue;
    rae_count += 1;
    if (rae_count > 1) break;
  }
  closedir(dir);
  if (rae_count == 0) return false;
  if (rae_count == 1) return true;
  DIR* dir2 = opendir(dir_path);
  if (!dir2) return false;
  size_t other_count = 0;
  struct dirent* entry2;
  while ((entry2 = readdir(dir2))) {
    if (strcmp(entry2->d_name, ".") == 0 || strcmp(entry2->d_name, "..") == 0) continue;
    char file_path[PATH_MAX];
    int written = snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, entry2->d_name);
    if (written <= 0 || (size_t)written >= sizeof(file_path)) continue;
    struct stat st;
    if (stat(file_path, &st) != 0) continue;
    if (!S_ISREG(st.st_mode)) continue;
    size_t len = strlen(file_path);
    if (len < 4 || strcmp(file_path + len - 4, ".rae") != 0) continue;
    if (strcmp(file_path, entry_file_path) == 0) continue;
    other_count += 1;
    if (other_count > 0) break;
  }
  closedir(dir2);
  return other_count == 0;
}

static bool should_auto_import(const char* entry_file_path) {
  char* dir_copy = strdup(entry_file_path);
  if (!dir_copy) {
    return false;
  }
  char* slash = strrchr(dir_copy, '/');
  if (!slash) {
    free(dir_copy);
    return false;
  }
  *slash = '\0';
  bool has_pack = directory_has_raepack(dir_copy);
  bool single_file = directory_has_only_entry_file(dir_copy, entry_file_path);
  free(dir_copy);
  return has_pack || single_file;
}

static bool module_graph_build(ModuleGraph* graph, const char* entry_file, uint64_t* hash_out) {
  char* resolved_entry = realpath(entry_file, NULL);
  if (!resolved_entry) {
    fprintf(stderr, "error: unable to resolve entry file '%s'\n", entry_file);
    return false;
  }
  char* module_path = derive_module_path(graph->root_path, resolved_entry);
  if (!module_path) {
    free(resolved_entry);
    return false;
  }
  bool ok = module_graph_load_module(graph, module_path, resolved_entry, NULL, hash_out);
  if (!ok) {
    free(module_path);
    free(resolved_entry);
    return false;
  }
  ModuleNode* entry_node = module_graph_find(graph, module_path);
  if (entry_node && entry_node->module && !entry_node->module->imports &&
      should_auto_import(entry_node->file_path)) {
    if (!auto_import_directory(graph, entry_node->file_path, hash_out)) {
      free(module_path);
      free(resolved_entry);
      return false;
    }
  }
  free(module_path);
  free(resolved_entry);
  return true;
}

static AstModule merge_module_graph(const ModuleGraph* graph) {
  AstModule merged = {.imports = NULL, .decls = NULL};
  AstDecl* tail = NULL;
  for (ModuleNode* node = graph->head; node; node = node->next) {
    AstDecl* decls = node->module ? node->module->decls : NULL;
    if (!decls) continue;
    if (!merged.decls) {
      merged.decls = decls;
    } else {
      tail->next = decls;
    }
    while (decls->next) {
      decls = decls->next;
    }
    tail = decls;
  }
  return merged;
}

static uint64_t hash_bytes(const char* data, size_t length) {
  const uint64_t fnv_offset = 1469598103934665603ull;
  const uint64_t fnv_prime = 1099511628211ull;
  uint64_t hash = fnv_offset;
  for (size_t i = 0; i < length; ++i) {
    hash ^= (uint64_t)(uint8_t)data[i];
    hash *= fnv_prime;
  }
  return hash;
}

static void print_usage(const char* prog) {
  fprintf(stderr, "Usage: %s <command> <file>\n", prog);
  fprintf(stderr, "\nCommands:\n");
  fprintf(stderr, "  lex <file>      Tokenize Rae source file\n");
  fprintf(stderr, "  parse <file>    Parse Rae source file and dump AST\n");
  fprintf(stderr, "  format <file>   Parse Rae source file and pretty-print it\n");
  fprintf(stderr, "  run <file>      Execute Rae source via the bytecode VM\n");
  fprintf(stderr, "  build <file>    Build Rae source (use --emit-c)\n");
}

static void dump_tokens(const TokenList* tokens) {
  for (size_t i = 0; i < tokens->count; ++i) {
    const Token* tok = &tokens->data[i];
    const char* name = token_kind_name(tok->kind);
    printf("%s \"", name);
    fwrite(tok->lexeme.data, 1, tok->lexeme.len, stdout);
    printf("\" at %zu:%zu\n", tok->line, tok->column);
  }
}

static bool compile_file_chunk(const char* file_path,
                               Chunk* chunk,
                               uint64_t* out_hash,
                               WatchSources* watch_sources) {
  Arena* arena = arena_create(1024 * 1024);
  if (!arena) {
    diag_fatal("could not allocate arena");
  }
  ModuleGraph graph;
  if (!module_graph_init(&graph, arena)) {
    arena_destroy(arena);
    return false;
  }
  WatchSources built_sources;
  watch_sources_init(&built_sources);
  uint64_t combined_hash = 0;
  uint64_t* hash_target = out_hash ? &combined_hash : NULL;
  if (!module_graph_build(&graph, file_path, hash_target)) {
    watch_sources_clear(&built_sources);
    module_graph_free(&graph);
    arena_destroy(arena);
    return false;
  }
  if (watch_sources) {
    if (!module_graph_collect_watch_sources(&graph, &built_sources)) {
      watch_sources_clear(&built_sources);
      module_graph_free(&graph);
      arena_destroy(arena);
      return false;
    }
  }
  AstModule merged = merge_module_graph(&graph);
  bool ok = vm_compile_module(&merged, chunk, file_path);
  module_graph_free(&graph);
  arena_destroy(arena);
  if (ok && out_hash) {
    *out_hash = combined_hash;
  }
  if (ok && watch_sources) {
    watch_sources_move(watch_sources, &built_sources);
  }
  watch_sources_clear(&built_sources);
  return ok;
}

static bool build_c_backend_output(const char* entry_file, const char* out_path) {
  Arena* arena = arena_create(1024 * 1024);
  if (!arena) {
    diag_fatal("could not allocate arena");
  }
  ModuleGraph graph;
  if (!module_graph_init(&graph, arena)) {
    arena_destroy(arena);
    return false;
  }
  bool ok = module_graph_build(&graph, entry_file, NULL);
  if (!ok) {
    module_graph_free(&graph);
    arena_destroy(arena);
    return false;
  }
  AstModule merged = merge_module_graph(&graph);
  ok = c_backend_emit(&merged, out_path);
  module_graph_free(&graph);
  arena_destroy(arena);
  return ok;
}

static int run_vm_file(const char* file_path) {
  Chunk chunk;
  if (!compile_file_chunk(file_path, &chunk, NULL, NULL)) {
    return 1;
  }

  VM vm;
  vm_init(&vm);
  VMResult result = vm_run(&vm, &chunk);
  chunk_free(&chunk);
  return result == VM_RUNTIME_OK ? 0 : 1;
}

static int run_command(const char* cmd, int argc, char** argv) {
  size_t file_size = 0;
  char* source = NULL;
  Arena* arena = NULL;
  const char* file_path = NULL;
  FormatOptions format_opts;
  bool is_format = (strcmp(cmd, "format") == 0);
  bool is_run = (strcmp(cmd, "run") == 0);
  bool is_build = (strcmp(cmd, "build") == 0);

  if (is_format) {
    if (!parse_format_args(argc, argv, &format_opts)) {
      return 1;
    }
    file_path = format_opts.input_path;
  } else if (strcmp(cmd, "lex") == 0 || strcmp(cmd, "parse") == 0) {
    if (argc < 1) {
      fprintf(stderr, "error: %s command requires a file argument\n", cmd);
      print_usage(argv[-1]);
      return 1;
    }
    file_path = argv[0];
  } else if (is_run) {
    RunOptions run_opts;
    if (!parse_run_args(argc, argv, &run_opts)) {
      print_usage(argv[-1]);
      return 1;
    }
    return run_opts.watch ? run_vm_watch(run_opts.input_path) : run_vm_file(run_opts.input_path);
  } else if (is_build) {
    BuildOptions build_opts;
    if (!parse_build_args(argc, argv, &build_opts)) {
      print_usage(argv[-1]);
      return 1;
    }
    if (!build_opts.emit_c) {
      fprintf(stderr, "error: build currently requires --emit-c\n");
      return 1;
    }
    if (!file_exists(build_opts.entry_path)) {
      fprintf(stderr, "error: entry file '%s' not found\n", build_opts.entry_path);
      return 1;
    }
    return build_c_backend_output(build_opts.entry_path, build_opts.out_path) ? 0 : 1;
  } else {
    fprintf(stderr, "error: unknown command '%s'\n", cmd);
    print_usage(argv[-1]);
    return 1;
  }

  source = read_file(file_path, &file_size);
  if (!source) {
    fprintf(stderr, "error: could not read file '%s'\n", file_path);
    return 1;
  }
  arena = arena_create(1024 * 1024);
  if (!arena) {
    free(source);
    diag_fatal("could not allocate arena");
  }
  TokenList tokens = lexer_tokenize(arena, file_path, source, file_size);

  if (strcmp(cmd, "lex") == 0) {
    dump_tokens(&tokens);
  } else if (strcmp(cmd, "parse") == 0) {
    AstModule* module = parse_module(arena, file_path, tokens);
    ast_dump_module(module, stdout);
  } else if (is_format) {
    AstModule* module = parse_module(arena, format_opts.input_path, tokens);
    if (format_opts.write_in_place || format_opts.output_path) {
      const char* output_path = format_opts.write_in_place ? format_opts.input_path : format_opts.output_path;
      FILE* out = fopen(output_path, "w");
      if (!out) {
        fprintf(stderr, "error: could not open '%s' for writing: %s\n", output_path, strerror(errno));
        arena_destroy(arena);
        free(source);
        return 1;
      }
      pretty_print_module(module, out);
      fclose(out);
    } else {
      pretty_print_module(module, stdout);
    }
  }

  arena_destroy(arena);
  free(source);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  const char* cmd = argv[1];
  if ((strcmp(cmd, "lex") == 0 || strcmp(cmd, "parse") == 0 || strcmp(cmd, "format") == 0 ||
       strcmp(cmd, "run") == 0 || strcmp(cmd, "build") == 0)) {
    return run_command(cmd, argc - 2, argv + 2);
  }

  fprintf(stderr, "error: unknown command '%s'\n", cmd);
  print_usage(argv[0]);
  return 1;
}
static time_t file_last_modified(const char* path) {
  struct stat info;
  if (stat(path, &info) != 0) {
    return (time_t)-1;
  }
  return info.st_mtime;
}

static void watch_state_init(WatchState* state, const char* fallback_path) {
  watch_sources_init(&state->sources);
  state->file_mtimes = NULL;
  state->dir_mtimes = NULL;
  state->fallback_path = fallback_path;
  time_t fallback_time = file_last_modified(fallback_path);
  if (fallback_time == (time_t)-1) {
    fallback_time = 0;
  }
  state->fallback_mtime = fallback_time;
}

static void watch_state_free(WatchState* state) {
  watch_sources_clear(&state->sources);
  free(state->file_mtimes);
  free(state->dir_mtimes);
  state->file_mtimes = NULL;
  state->dir_mtimes = NULL;
}

static bool allocate_time_array(time_t** array, size_t count) {
  if (count == 0) {
    free(*array);
    *array = NULL;
    return true;
  }
  time_t* resized = realloc(*array, count * sizeof(time_t));
  if (!resized) {
    return false;
  }
  *array = resized;
  return true;
}

static bool watch_state_apply_sources(WatchState* state, WatchSources* new_sources) {
  if (!allocate_time_array(&state->file_mtimes, new_sources->file_count)) {
    return false;
  }
  if (!allocate_time_array(&state->dir_mtimes, new_sources->dir_count)) {
    return false;
  }
  for (size_t i = 0; i < new_sources->file_count; ++i) {
    time_t modified = file_last_modified(new_sources->files[i]);
    state->file_mtimes[i] = (modified == (time_t)-1) ? 0 : modified;
  }
  for (size_t i = 0; i < new_sources->dir_count; ++i) {
    time_t modified = file_last_modified(new_sources->dirs[i]);
    state->dir_mtimes[i] = (modified == (time_t)-1) ? 0 : modified;
  }
  watch_sources_move(&state->sources, new_sources);
  time_t fallback_time = file_last_modified(state->fallback_path);
  if (fallback_time == (time_t)-1) {
    fallback_time = 0;
  }
  state->fallback_mtime = fallback_time;
  return true;
}

static time_t wait_for_stable_timestamp(const char* path, time_t initial) {
  if (initial == (time_t)-1) {
    return initial;
  }
  time_t current = initial;
  int stable_checks = 0;
  while (stable_checks < 3) {
    usleep(200000);
    time_t verify = file_last_modified(path);
    if (verify != current) {
      current = verify;
      stable_checks = 0;
      continue;
    }
    stable_checks += 1;
  }
  return current;
}

static const char* watch_state_poll_change(WatchState* state) {
  for (size_t i = 0; i < state->sources.file_count; ++i) {
    const char* path = state->sources.files[i];
    time_t current = file_last_modified(path);
    if (current == state->file_mtimes[i]) {
      continue;
    }
    time_t confirmed = wait_for_stable_timestamp(path, current);
    state->file_mtimes[i] = confirmed;
    return path;
  }
  for (size_t i = 0; i < state->sources.dir_count; ++i) {
    const char* path = state->sources.dirs[i];
    time_t current = file_last_modified(path);
    if (current == state->dir_mtimes[i]) {
      continue;
    }
    time_t confirmed = wait_for_stable_timestamp(path, current);
    state->dir_mtimes[i] = confirmed;
    return path;
  }
  if (state->fallback_path) {
    time_t current = file_last_modified(state->fallback_path);
    if (current != state->fallback_mtime) {
      time_t confirmed = wait_for_stable_timestamp(state->fallback_path, current);
      state->fallback_mtime = confirmed;
      return state->fallback_path;
    }
  }
  return NULL;
}

static const char* watch_state_wait_for_change(WatchState* state) {
  for (;;) {
    sleep(1);
    const char* changed = watch_state_poll_change(state);
    if (changed) {
      return changed;
    }
  }
}

static int run_vm_watch(const char* file_path) {
  printf("Watching '%s' for changes (Ctrl+C to exit)\n", file_path);
  fflush(stdout);

  VmRegistry registry;
  vm_registry_init(&registry);
  int exit_code = 0;
  int reload_count = 0;
  bool has_hash = false;
  uint64_t last_hash = 0;
  WatchState watch_state;
  watch_state_init(&watch_state, file_path);

  for (;;) {
    uint64_t file_hash = 0;
    Chunk chunk;
    WatchSources new_sources;
    watch_sources_init(&new_sources);
    if (!compile_file_chunk(file_path, &chunk, &file_hash, &new_sources)) {
      fprintf(stderr, "[watch] compile failed. Waiting for changes...\n");
    } else {
      if (!watch_state_apply_sources(&watch_state, &new_sources)) {
        fprintf(stderr,
                "[watch] warning: failed to update watch list; changes in helper files may be "
                "missed until the next successful run.\n");
      }
      if (has_hash && file_hash == last_hash) {
        chunk_free(&chunk);
        printf("[watch] no code changes detected.\n");
        fflush(stdout);
      } else {
        has_hash = true;
        last_hash = file_hash;
        vm_registry_reload(&registry, file_path, chunk);
        VmModule* module = vm_registry_find(&registry, file_path);
        if (module) {
          VM vm;
          vm_init(&vm);
          reload_count += 1;
          printf("[watch] running latest version... (reload #%d)\n", reload_count);
          fflush(stdout);
          VMResult result = vm_run(&vm, &module->chunk);
          if (result != VM_RUNTIME_OK) {
            exit_code = 1;
          }
        }
      }
    }
    watch_sources_clear(&new_sources);

    const char* changed_path = watch_state_wait_for_change(&watch_state);
    if (!changed_path) {
      changed_path = file_path;
    }
    printf("[watch] change detected in %s. Recompiling...\n", changed_path);
    fflush(stdout);
  }

  vm_registry_free(&registry);
  watch_state_free(&watch_state);
  return exit_code;
}
