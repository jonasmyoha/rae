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
#ifndef RAE_RUNTIME_SOURCE_DIR
#define RAE_RUNTIME_SOURCE_DIR "runtime"
#endif
#include "arena.h"
#include "str.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "pretty.h"
#include "c_backend.h"
#include "raepack.h"
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
  const char* project_path;
  bool emit_c;
  int target;
  int profile;
} BuildOptions;

typedef struct {
  const char* file_path;
  const char* target_id;
  bool json;
} PackOptions;

typedef enum {
  BUILD_TARGET_COMPILED = 0,
  BUILD_TARGET_LIVE,
  BUILD_TARGET_HYBRID
} BuildTarget;

typedef enum {
  BUILD_PROFILE_RELEASE = 0,
  BUILD_PROFILE_DEV
} BuildProfile;

typedef struct {
  int64_t next;
} TickCounter;

static bool native_next_tick(struct VM* vm,
                             VmNativeResult* out_result,
                             const Value* args,
                             size_t arg_count,
                             void* user_data) {
  (void)vm;
  (void)args;
  if (!out_result || !user_data) {
    diag_error(NULL, 0, 0, "nextTick native state missing");
    return false;
  }
  if (arg_count != 0) {
    diag_error(NULL, 0, 0, "nextTick expects no arguments");
    return false;
  }
  TickCounter* counter = (TickCounter*)user_data;
  counter->next += 1;
  out_result->has_value = true;
  out_result->value = value_int(counter->next);
  return true;
}

static bool native_sleep_ms(struct VM* vm,
                            VmNativeResult* out_result,
                            const Value* args,
                            size_t arg_count,
                            void* user_data) {
  (void)vm;
  (void)user_data;
  if (!out_result) {
    diag_error(NULL, 0, 0, "sleepMs native state missing");
    return false;
  }
  out_result->has_value = false;
  if (arg_count != 1) {
    diag_error(NULL, 0, 0, "sleepMs expects exactly one argument");
    return false;
  }
  if (args[0].type != VAL_INT) {
    diag_error(NULL, 0, 0, "sleepMs expects an integer duration in milliseconds");
    return false;
  }
  int64_t ms = args[0].as.int_value;
  if (ms <= 0) {
    return true;
  }
  usleep((useconds_t)ms * 1000);
  return true;
}

static bool register_default_natives(VmRegistry* registry, TickCounter* tick_counter) {
  if (!registry) return false;
  bool ok = true;
  if (tick_counter) {
    ok = vm_registry_register_native(registry, "nextTick", native_next_tick, tick_counter) && ok;
  }
  ok = vm_registry_register_native(registry, "sleepMs", native_sleep_ms, NULL) && ok;
  return ok;
}

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
static bool build_c_backend_output(const char* entry_file,
                                   const char* project_root,
                                   const char* out_path);
static bool build_vm_output(const char* entry_file,
                            const char* project_root,
                            const char* out_path);
static bool ensure_directory_tree(const char* dir_path);
static bool ensure_parent_directory(const char* file_path);
static bool copy_runtime_assets(const char* dest_dir);
static bool write_function_manifest(const AstModule* module, const char* out_c_path);
static bool write_c_backend_stub(const AstModule* module, const char* out_path);
static char* derive_entry_stem(const char* entry_file);
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
  opts->out_path = NULL;
  opts->project_path = NULL;
  opts->emit_c = false;
  opts->target = BUILD_TARGET_COMPILED;
  opts->profile = BUILD_PROFILE_RELEASE;

  const char* entry_from_flag = NULL;
  const char* entry_positional = NULL;

  int i = 0;
  while (i < argc) {
    const char* arg = argv[i];
    if (strcmp(arg, "--emit-c") == 0) {
      opts->emit_c = true;
      i += 1;
      continue;
    }
    if (strcmp(arg, "--target") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --target expects one of live|compiled|hybrid\n");
        return false;
      }
      const char* value = argv[i + 1];
      if (strcmp(value, "live") == 0) {
        opts->target = BUILD_TARGET_LIVE;
      } else if (strcmp(value, "compiled") == 0) {
        opts->target = BUILD_TARGET_COMPILED;
      } else if (strcmp(value, "hybrid") == 0) {
        opts->target = BUILD_TARGET_HYBRID;
      } else {
        fprintf(stderr,
                "error: unknown target '%s' (expected live|compiled|hybrid)\n",
                value);
        return false;
      }
      i += 2;
      continue;
    }
    if (strcmp(arg, "--profile") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --profile expects dev or release\n");
        return false;
      }
      const char* value = argv[i + 1];
      if (strcmp(value, "dev") == 0) {
        opts->profile = BUILD_PROFILE_DEV;
      } else if (strcmp(value, "release") == 0) {
        opts->profile = BUILD_PROFILE_RELEASE;
      } else {
        fprintf(stderr, "error: unknown profile '%s' (expected dev|release)\n", value);
        return false;
      }
      i += 2;
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
    if (strcmp(arg, "--project") == 0 || strcmp(arg, "-p") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: %s expects a directory\n", arg);
        return false;
      }
      opts->project_path = argv[i + 1];
      i += 2;
      continue;
    }
    if (strcmp(arg, "--entry") == 0 || strcmp(arg, "-e") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: %s expects a file path\n", arg);
        return false;
      }
      if (entry_from_flag) {
        fprintf(stderr, "error: --entry specified multiple times ('%s' and '%s')\n",
                entry_from_flag, argv[i + 1]);
        return false;
      }
      entry_from_flag = argv[i + 1];
      i += 2;
      continue;
    }
    if (arg[0] == '-') {
      fprintf(stderr, "error: unknown build option '%s'\n", arg);
      return false;
    }
    if (entry_positional) {
      fprintf(stderr, "error: multiple entry files provided ('%s' and '%s')\n", entry_positional,
              arg);
      return false;
    }
    entry_positional = arg;
    i += 1;
  }

  if (entry_from_flag && entry_positional) {
    fprintf(stderr, "error: specify entry file either positionally or via --entry, not both\n");
    return false;
  }
  opts->entry_path = entry_from_flag ? entry_from_flag : entry_positional;
  if (!opts->entry_path) {
    fprintf(stderr, "error: build command requires an entry file argument\n");
    return false;
  }
  if (!opts->out_path) {
    switch (opts->target) {
      case BUILD_TARGET_LIVE:
        opts->out_path = "build/out.vmchunk";
        break;
      case BUILD_TARGET_HYBRID:
        opts->out_path = "build/out.hybrid";
        break;
      case BUILD_TARGET_COMPILED:
      default:
        opts->out_path = "build/out.c";
        break;
    }
  }
  return true;
}

static bool parse_pack_args(int argc, char** argv, PackOptions* opts) {
  opts->file_path = NULL;
  opts->target_id = NULL;
  opts->json = false;

  int i = 0;
  while (i < argc) {
    const char* arg = argv[i];
    if (strcmp(arg, "--json") == 0) {
      opts->json = true;
      i += 1;
      continue;
    }
    if (strcmp(arg, "--target") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --target expects a target id\n");
        return false;
      }
      opts->target_id = argv[i + 1];
      i += 2;
      continue;
    }
    if (arg[0] == '-') {
      fprintf(stderr, "error: unknown pack option '%s'\n", arg);
      return false;
    }
    if (opts->file_path) {
      fprintf(stderr, "error: multiple pack files provided ('%s' and '%s')\n",
              opts->file_path,
              arg);
      return false;
    }
    opts->file_path = arg;
    i += 1;
  }

  if (!opts->file_path) {
    fprintf(stderr, "error: pack command requires a file argument\n");
    return false;
  }
  return true;
}

static bool file_exists(const char* path) {
  if (!path) return false;
  struct stat st;
  return stat(path, &st) == 0;
}

static bool directory_exists(const char* path) {
  if (!path) return false;
  struct stat st;
  if (stat(path, &st) != 0) {
    return false;
  }
  return S_ISDIR(st.st_mode);
}

static bool ensure_directory_component(const char* path) {
  if (!path || path[0] == '\0' || strcmp(path, ".") == 0 || strcmp(path, "..") == 0) {
    return true;
  }
  struct stat st;
  if (stat(path, &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      return true;
    }
    fprintf(stderr, "error: '%s' exists but is not a directory\n", path);
    return false;
  }
  if (mkdir(path, 0755) == 0) {
    return true;
  }
  if (errno == EEXIST) {
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      return true;
    }
  }
  fprintf(stderr, "error: could not create directory '%s': %s\n", path, strerror(errno));
  return false;
}

static bool ensure_directory_tree(const char* dir_path) {
  if (!dir_path || dir_path[0] == '\0') {
    return true;
  }
  char path_copy[PATH_MAX];
  size_t len = strlen(dir_path);
  if (len >= sizeof(path_copy)) {
    fprintf(stderr, "error: directory path too long\n");
    return false;
  }
  memcpy(path_copy, dir_path, len + 1);
  for (char* cursor = path_copy + 1; *cursor; ++cursor) {
    if (*cursor == '/') {
      *cursor = '\0';
      if (!ensure_directory_component(path_copy)) {
        return false;
      }
      *cursor = '/';
    }
  }
  return ensure_directory_component(path_copy);
}

static bool ensure_parent_directory(const char* file_path) {
  if (!file_path || file_path[0] == '\0') {
    return false;
  }
  const char* slash = strrchr(file_path, '/');
  if (!slash) {
    return true;
  }
  size_t dir_len = (size_t)(slash - file_path);
  if (dir_len == 0) {
    return true;
  }
  char dir_path[PATH_MAX];
  if (dir_len >= sizeof(dir_path)) {
    fprintf(stderr, "error: output path too long\n");
    return false;
  }
  memcpy(dir_path, file_path, dir_len);
  dir_path[dir_len] = '\0';
  return ensure_directory_tree(dir_path);
}

static char* derive_entry_stem(const char* entry_file) {
  if (!entry_file) {
    return NULL;
  }
  const char* slash = strrchr(entry_file, '/');
#ifdef _WIN32
  const char* slash_win = strrchr(entry_file, '\\');
  if (!slash || (slash_win && slash_win > slash)) {
    slash = slash_win;
  }
#endif
  const char* name = slash ? slash + 1 : entry_file;
  size_t len = strlen(name);
  if (len > 4 && strcmp(name + len - 4, ".rae") == 0) {
    len -= 4;
  }
  if (len == 0) {
    len = strlen(name);
  }
  char* stem = malloc(len + 1);
  if (!stem) {
    return NULL;
  }
  memcpy(stem, name, len);
  stem[len] = '\0';
  return stem;
}

static bool copy_stream(FILE* src, FILE* dest) {
  char buffer[4096];
  while (!feof(src)) {
    size_t read_bytes = fread(buffer, 1, sizeof(buffer), src);
    if (read_bytes > 0) {
      if (fwrite(buffer, 1, read_bytes, dest) != read_bytes) {
        return false;
      }
    }
    if (ferror(src)) {
      return false;
    }
  }
  return fflush(dest) == 0;
}

static bool copy_file_to(const char* src_path, const char* dest_path) {
  FILE* src = fopen(src_path, "rb");
  if (!src) {
    fprintf(stderr, "error: could not open runtime source '%s': %s\n", src_path, strerror(errno));
    return false;
  }
  FILE* dest = fopen(dest_path, "wb");
  if (!dest) {
    fprintf(stderr, "error: could not open '%s' for writing: %s\n", dest_path, strerror(errno));
    fclose(src);
    return false;
  }
  bool ok = copy_stream(src, dest);
  fclose(src);
  fclose(dest);
  return ok;
}

static bool copy_runtime_file(const char* dest_dir, const char* file_name) {
  char src_path[PATH_MAX];
  char dest_path[PATH_MAX];
  if (snprintf(src_path, sizeof(src_path), "%s/%s", RAE_RUNTIME_SOURCE_DIR, file_name) >=
      (int)sizeof(src_path)) {
    fprintf(stderr, "error: runtime source path too long\n");
    return false;
  }
  if (snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_dir, file_name) >=
      (int)sizeof(dest_path)) {
    fprintf(stderr, "error: runtime destination path too long\n");
    return false;
  }
  return copy_file_to(src_path, dest_path);
}

static bool copy_runtime_assets(const char* dest_dir) {
  if (!dest_dir || dest_dir[0] == '\0') {
    dest_dir = ".";
  }
  if (!copy_runtime_file(dest_dir, "rae_runtime.h")) {
    return false;
  }
  if (!copy_runtime_file(dest_dir, "rae_runtime.c")) {
    return false;
  }
  return true;
}

static bool write_bytes(FILE* out, const void* data, size_t size) {
  if (!out || !data || size == 0) {
    return size == 0;
  }
  return fwrite(data, 1, size, out) == size;
}

static bool write_u32(FILE* out, uint32_t value) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(value & 0xFF);
  bytes[1] = (uint8_t)((value >> 8) & 0xFF);
  bytes[2] = (uint8_t)((value >> 16) & 0xFF);
  bytes[3] = (uint8_t)((value >> 24) & 0xFF);
  return write_bytes(out, bytes, sizeof(bytes));
}

static bool write_u8(FILE* out, uint8_t value) {
  return write_bytes(out, &value, sizeof(value));
}

static bool write_i64(FILE* out, int64_t value) {
  uint8_t bytes[8];
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    bytes[i] = (uint8_t)((value >> (i * 8)) & 0xFF);
  }
  return write_bytes(out, bytes, sizeof(bytes));
}

static bool write_vm_chunk_file(const Chunk* chunk, const char* out_path) {
  if (!chunk || !out_path) {
    return false;
  }
  if (chunk->code_count > UINT32_MAX || chunk->constants_count > UINT32_MAX) {
    fprintf(stderr, "error: VM chunk too large to serialize\n");
    return false;
  }
  FILE* out = fopen(out_path, "wb");
  if (!out) {
    fprintf(stderr, "error: could not open '%s' for writing: %s\n", out_path, strerror(errno));
    return false;
  }
  bool ok = true;
  const uint8_t magic[4] = {'R', 'V', 'M', '1'};
  ok = ok && write_bytes(out, magic, sizeof(magic));
  ok = ok && write_u32(out, 1);  // format version
  uint32_t constant_count = (uint32_t)chunk->constants_count;
  ok = ok && write_u32(out, constant_count);
  for (size_t i = 0; ok && i < chunk->constants_count; ++i) {
    const Value* value = &chunk->constants[i];
    ok = ok && write_u8(out, (uint8_t)value->type);
    switch (value->type) {
      case VAL_INT:
        ok = ok && write_i64(out, value->as.int_value);
        break;
      case VAL_BOOL:
        ok = ok && write_u8(out, value->as.bool_value ? 1 : 0);
        break;
      case VAL_STRING: {
        size_t len = value->as.string_value.length;
        if (len > UINT32_MAX) {
          fprintf(stderr, "error: string constant too long for VM chunk\n");
          ok = false;
          break;
        }
        const char* data = value->as.string_value.chars ? value->as.string_value.chars : "";
        ok = ok && write_u32(out, (uint32_t)len);
        if (len > 0) {
          ok = ok && write_bytes(out, data, len);
        }
        break;
      }
      default:
        fprintf(stderr, "error: unknown VM constant type\n");
        ok = false;
        break;
    }
  }
  uint32_t code_size = (uint32_t)chunk->code_count;
  ok = ok && write_u32(out, code_size);
  if (code_size > 0) {
    ok = ok && write_bytes(out, chunk->code, chunk->code_count);
  }
  uint32_t line_count = chunk->lines ? (uint32_t)chunk->code_count : 0;
  ok = ok && write_u32(out, line_count);
  if (chunk->lines) {
    for (size_t i = 0; ok && i < chunk->code_count; ++i) {
      int line = chunk->lines[i];
      uint32_t line_value = line < 0 ? 0u : (uint32_t)line;
      ok = ok && write_u32(out, line_value);
    }
  }
  if (fclose(out) != 0) {
    ok = false;
  }
  if (!ok) {
    fprintf(stderr, "error: failed to write VM bytecode to '%s'\n", out_path);
  }
  return ok;
}

static char* type_ref_to_cstr(const AstTypeRef* type) {
  if (!type || !type->parts) {
    return strdup("Any");
  }
  size_t total = 0;
  size_t segments = 0;
  const AstIdentifierPart* part = type->parts;
  while (part) {
    total += part->text.len;
    segments += 1;
    part = part->next;
  }
  size_t len = total + (segments > 1 ? segments - 1 : 0);
  char* result = malloc(len + 1);
  if (!result) {
    return NULL;
  }
  size_t offset = 0;
  part = type->parts;
  while (part) {
    memcpy(result + offset, part->text.data, part->text.len);
    offset += part->text.len;
    if (part->next) {
      result[offset++] = '.';
    }
    part = part->next;
  }
  result[offset] = '\0';
  return result;
}

static char* derive_manifest_path(const char* out_path) {
  if (!out_path) return NULL;
  size_t len = strlen(out_path);
  const char* dot = strrchr(out_path, '.');
  size_t base_len = dot ? (size_t)(dot - out_path) : len;
  const char* suffix = ".manifest.json";
  size_t suffix_len = strlen(suffix);
  char* path = malloc(base_len + suffix_len + 1);
  if (!path) return NULL;
  memcpy(path, out_path, base_len);
  memcpy(path + base_len, suffix, suffix_len + 1);
  return path;
}

static bool write_function_manifest(const AstModule* module, const char* out_c_path) {
  if (!module || !out_c_path) {
    return false;
  }
  char* manifest_path = derive_manifest_path(out_c_path);
  if (!manifest_path) {
    fprintf(stderr, "error: unable to derive manifest path for '%s'\n", out_c_path);
    return false;
  }
  FILE* out = fopen(manifest_path, "w");
  if (!out) {
    fprintf(stderr, "error: unable to open manifest '%s'\n", manifest_path);
    free(manifest_path);
    return false;
  }
  fprintf(out, "{\n  \"functions\": [\n");
  const AstDecl* decl = module->decls;
  int first = 1;
  while (decl) {
    if (decl->kind == AST_DECL_FUNC) {
      const AstFuncDecl* fn = &decl->as.func_decl;
      if (!first) {
        fprintf(out, ",\n");
      }
      first = 0;
      char* name = str_to_cstr(fn->name);
      const char* kind = fn->is_extern ? "extern" : "rae";
      fprintf(out, "    {\n      \"name\": \"%s\",\n      \"kind\": \"%s\",\n      \"params\": [",
              name ? name : "",
              kind);
      const AstParam* param = fn->params;
      int first_param = 1;
      while (param) {
        if (!first_param) {
          fprintf(out, ", ");
        }
        char* param_name = str_to_cstr(param->name);
        char* type_str = type_ref_to_cstr(param->type);
        fprintf(out, "{\"name\": \"%s\", \"type\": \"%s\"}",
                param_name ? param_name : "",
                type_str ? type_str : "Any");
        free(param_name);
        free(type_str);
        first_param = 0;
        param = param->next;
      }
      fprintf(out, "],\n      \"returns\": [");
      const AstReturnItem* ret = fn->returns;
      int first_ret = 1;
      while (ret) {
        if (!first_ret) {
          fprintf(out, ", ");
        }
        char* type_str = type_ref_to_cstr(ret->type);
        fprintf(out, "\"%s\"", type_str ? type_str : "Any");
        free(type_str);
        first_ret = 0;
        ret = ret->next;
      }
      fprintf(out, "]\n    }");
      free(name);
    }
    decl = decl->next;
  }
  fprintf(out, "\n  ]\n}\n");
  fclose(out);
  free(manifest_path);
  return true;
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


static bool module_graph_init(ModuleGraph* graph, Arena* arena, const char* project_root) {
  memset(graph, 0, sizeof(*graph));
  graph->arena = arena;
  char* resolved = NULL;
  bool root_from_cwd = (project_root == NULL || project_root[0] == '\0');
  if (!root_from_cwd) {
    resolved = realpath(project_root, NULL);
    if (!resolved) {
      fprintf(stderr, "error: unable to resolve project path '%s'\n", project_root);
      return false;
    }
  } else {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
      perror("getcwd");
      return false;
    }
    resolved = realpath(cwd, NULL);
    if (!resolved) {
      resolved = strdup(cwd);
    }
  }
  graph->root_path = resolved;
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
  if (root_from_cwd && last_sep) {
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

static char* try_resolve_lib_module(const char* root, const char* normalized) {
  // Check root/lib/normalized.rae
  size_t root_len = strlen(root);
  size_t mod_len = strlen(normalized);
  size_t total = root_len + mod_len + 10; // /lib/ .rae
  char* buffer = malloc(total);
  if (!buffer) return NULL;
  
  if (root_len > 0 && (root[root_len - 1] == '/' || root[root_len - 1] == '\\')) {
    root_len -= 1;
  }
  snprintf(buffer, total, "%.*s/lib/%s.rae", (int)root_len, root, normalized);
  
  if (file_exists(buffer)) {
    return buffer;
  }
  free(buffer);
  return NULL;
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
      char* lib_file = try_resolve_lib_module(graph->root_path, normalized);
      if (lib_file) {
        free(child_file);
        child_file = lib_file;
      } else {
        fprintf(stderr, "error: imported module '%s' not found (required by '%s')\n", normalized,
                module_path ? module_path : "<entry>");
        module_stack_print_trace(&frame, normalized);
        free(normalized);
        free(child_file);
        return false;
      }
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
  fprintf(stderr, "  pack <file>     Validate and summarize a .raepack file\n");
  fprintf(stderr, "                 (options: --json, --target <id>)\n");
  fprintf(stderr,
          "  build [opts]    Build Rae source (--emit-c required for now)\n");
  fprintf(stderr,
          "                  Options: --entry <file>, --project <dir>, --out <file>\n");
  fprintf(stderr,
          "                           --target <live|compiled|hybrid>, --profile <dev|release>\n");
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

static void print_str(Str value) {
  if (value.len == 0 || !value.data) return;
  fwrite(value.data, 1, value.len, stdout);
}

static void print_json_string(Str value) {
  putchar('"');
  for (size_t i = 0; i < value.len; ++i) {
    unsigned char c = (unsigned char)value.data[i];
    switch (c) {
      case '"':
        fputs("\\\"", stdout);
        break;
      case '\\':
        fputs("\\\\", stdout);
        break;
      case '\n':
        fputs("\\n", stdout);
        break;
      case '\r':
        fputs("\\r", stdout);
        break;
      case '\t':
        fputs("\\t", stdout);
        break;
      default:
        if (c < 0x20) {
          printf("\\u%04x", c);
        } else {
          putchar(c);
        }
        break;
    }
  }
  putchar('"');
}

static void dump_raepack(const RaePack* pack) {
  if (!pack) return;
  printf("Pack ");
  print_str(pack->name);
  printf("\n");
  printf("Format: ");
  print_str(pack->format);
  printf("\n");
  printf("Version: %lld\n", (long long)pack->version);
  printf("Default target: ");
  print_str(pack->default_target);
  printf("\n");
  printf("Targets:\n");
  for (const RaePackTarget* target = pack->targets; target; target = target->next) {
    printf("- ");
    print_str(target->id);
    printf(" (");
    print_str(target->label);
    printf(")\n");
    printf("  entry: ");
    print_str(target->entry);
    printf("\n");
    printf("  sources:\n");
    for (const RaePackSource* source = target->sources; source; source = source->next) {
      printf("    - ");
      print_str(source->path);
      printf(" [%s]\n", raepack_emit_name(source->emit));
    }
  }
}

static void dump_raepack_json(const RaePack* pack, const RaePackTarget* selected_target) {
  if (!pack) return;
  printf("{\n");
  printf("  \"name\": ");
  print_json_string(pack->name);
  printf(",\n");
  printf("  \"format\": ");
  print_json_string(pack->format);
  printf(",\n");
  printf("  \"version\": %lld,\n", (long long)pack->version);
  printf("  \"defaultTarget\": ");
  print_json_string(pack->default_target);
  printf(",\n");
  printf("  \"targets\": [\n");
  for (const RaePackTarget* target = pack->targets; target; target = target->next) {
    printf("    {\n");
    printf("      \"id\": ");
    print_json_string(target->id);
    printf(",\n");
    printf("      \"label\": ");
    print_json_string(target->label);
    printf(",\n");
    printf("      \"entry\": ");
    print_json_string(target->entry);
    printf(",\n");
    printf("      \"sources\": [\n");
    for (const RaePackSource* source = target->sources; source; source = source->next) {
      printf("        {\"path\": ");
      print_json_string(source->path);
      printf(", \"emit\": ");
      print_json_string(str_from_cstr(raepack_emit_name(source->emit)));
      printf("}%s\n", source->next ? "," : "");
    }
    printf("      ]\n");
    printf("    }%s\n", target->next ? "," : "");
  }
  printf("  ]");
  if (selected_target) {
    printf(",\n  \"selectedTarget\": ");
    print_json_string(selected_target->id);
  }
  printf("\n}\n");
}

static int run_raepack_file(const PackOptions* opts) {
  if (!opts || !opts->file_path) return 1;
  RaePack pack;
  if (!raepack_parse_file(opts->file_path, &pack)) {
    return 1;
  }
  const RaePackTarget* selected = NULL;
  if (opts->target_id && opts->target_id[0] != '\0') {
    selected = raepack_find_target(&pack, str_from_cstr(opts->target_id));
    if (!selected) {
      fprintf(stderr, "error: target '%s' not found in '%s'\n",
              opts->target_id,
              opts->file_path);
      raepack_free(&pack);
      return 1;
    }
  }
  if (opts->json) {
    dump_raepack_json(&pack, selected);
  } else {
    dump_raepack(&pack);
  }
  raepack_free(&pack);
  return 0;
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
  if (!module_graph_init(&graph, arena, NULL)) {
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

static bool build_c_backend_output(const char* entry_file,
                                   const char* project_root,
                                   const char* out_path) {
  if (!out_path || out_path[0] == '\0') {
    fprintf(stderr, "error: build requires a valid output path\n");
    return false;
  }
  if (!ensure_parent_directory(out_path)) {
    return false;
  }
  char out_dir[PATH_MAX];
  const char* slash = strrchr(out_path, '/');
  if (!slash) {
    strncpy(out_dir, ".", sizeof(out_dir));
    out_dir[sizeof(out_dir) - 1] = '\0';
  } else {
    size_t dir_len = (size_t)(slash - out_path);
    if (dir_len == 0) {
      strncpy(out_dir, "/", sizeof(out_dir));
      out_dir[sizeof(out_dir) - 1] = '\0';
    } else {
      if (dir_len >= sizeof(out_dir)) {
        fprintf(stderr, "error: output directory path too long\n");
        return false;
      }
      memcpy(out_dir, out_path, dir_len);
      out_dir[dir_len] = '\0';
    }
  }
  Arena* arena = arena_create(1024 * 1024);
  if (!arena) {
    diag_fatal("could not allocate arena");
  }
  ModuleGraph graph;
  if (!module_graph_init(&graph, arena, project_root)) {
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
  if (ok) {
    ok = copy_runtime_assets(out_dir);
  } else {
    fprintf(stderr,
            "note: Rae C backend is not finished, writing stub C output to '%s'.\n"
            "note: See docs/c-backend-plan.md for the roadmap.\n",
            out_path);
    ok = write_c_backend_stub(&merged, out_path);
  }
  if (ok) {
    ok = write_function_manifest(&merged, out_path);
  }
  module_graph_free(&graph);
  arena_destroy(arena);
  return ok;
}

static bool write_c_backend_stub(const AstModule* module, const char* out_path) {
  if (!module || !out_path) {
    return false;
  }
  FILE* out = fopen(out_path, "w");
  if (!out) {
    fprintf(stderr, "error: unable to open '%s' for stub C output\n", out_path);
    return false;
  }
  bool ok = true;
  if (fprintf(out,
              "/* Rae C backend stub output */\n"
              "/* See docs/c-backend-plan.md for the roadmap. */\n"
              "#include <stdio.h>\n\n") < 0) {
    ok = false;
  }
  size_t func_count = 0;
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_FUNC) {
      func_count += 1;
    }
  }
  if (ok) {
    if (func_count == 0) {
      if (fprintf(out, "/* No functions discovered in entry module. */\n") < 0) {
        ok = false;
      }
    } else {
      if (fprintf(out, "/* Rae functions discovered by the frontend: */\n") < 0) {
        ok = false;
      }
      for (const AstDecl* decl = module->decls; decl && ok; decl = decl->next) {
        if (decl->kind != AST_DECL_FUNC) {
          continue;
        }
        char* name = str_to_cstr(decl->as.func_decl.name);
        if (!name) {
          ok = false;
          break;
        }
        if (fprintf(out, "/*   - func %s */\n", name) < 0) {
          ok = false;
        }
        free(name);
      }
    }
  }
  if (ok) {
    if (fprintf(out,
                "\nint main(void) {\n"
                "  fprintf(stderr, \"Rae C backend stub: see docs/c-backend-plan.md.\\\\n\");\n"
                "  fprintf(stderr, \"Parsed %zu Rae function(s); codegen not ready yet.\\\\n\");\n"
                "  return 1;\n"
                "}\n",
                func_count) < 0) {
      ok = false;
    }
  }
  if (fclose(out) != 0) {
    ok = false;
  }
  if (!ok) {
    remove(out_path);
  }
  return ok;
}

static bool build_vm_output(const char* entry_file,
                            const char* project_root,
                            const char* out_path) {
  if (!out_path || out_path[0] == '\0') {
    fprintf(stderr, "error: build requires a valid output path\n");
    return false;
  }
  if (!ensure_parent_directory(out_path)) {
    return false;
  }
  Arena* arena = arena_create(1024 * 1024);
  if (!arena) {
    diag_fatal("could not allocate arena");
  }
  ModuleGraph graph;
  if (!module_graph_init(&graph, arena, project_root)) {
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
  Chunk chunk;
  chunk_init(&chunk);
  ok = vm_compile_module(&merged, &chunk, entry_file);
  if (ok) {
    ok = write_vm_chunk_file(&chunk, out_path);
  }
  if (ok) {
    ok = write_function_manifest(&merged, out_path);
  }
  chunk_free(&chunk);
  module_graph_free(&graph);
  arena_destroy(arena);
  return ok;
}

static bool build_hybrid_output(const char* entry_file,
                                const char* project_root,
                                const char* out_path) {
  if (!out_path || out_path[0] == '\0') {
    fprintf(stderr, "error: build requires a valid output path\n");
    return false;
  }
  if (!ensure_directory_tree(out_path)) {
    return false;
  }
  char vm_dir[PATH_MAX];
  char compiled_dir[PATH_MAX];
  if (snprintf(vm_dir, sizeof(vm_dir), "%s/vm", out_path) >= (int)sizeof(vm_dir)) {
    fprintf(stderr, "error: hybrid vm directory path too long\n");
    return false;
  }
  if (snprintf(compiled_dir, sizeof(compiled_dir), "%s/compiled", out_path) >=
      (int)sizeof(compiled_dir)) {
    fprintf(stderr, "error: hybrid compiled directory path too long\n");
    return false;
  }
  if (!ensure_directory_tree(vm_dir) || !ensure_directory_tree(compiled_dir)) {
    return false;
  }
  char* entry_stem = derive_entry_stem(entry_file);
  if (!entry_stem) {
    fprintf(stderr, "error: unable to derive entry stem for hybrid build\n");
    return false;
  }
  char chunk_path[PATH_MAX];
  char c_path[PATH_MAX];
  if (snprintf(chunk_path, sizeof(chunk_path), "%s/%s.vmchunk", vm_dir, entry_stem) >=
      (int)sizeof(chunk_path)) {
    fprintf(stderr, "error: hybrid VM chunk path too long\n");
    free(entry_stem);
    return false;
  }
  if (snprintf(c_path, sizeof(c_path), "%s/%s.c", compiled_dir, entry_stem) >=
      (int)sizeof(c_path)) {
    fprintf(stderr, "error: hybrid C output path too long\n");
    free(entry_stem);
    return false;
  }
  free(entry_stem);

  Arena* arena = arena_create(1024 * 1024);
  if (!arena) {
    diag_fatal("could not allocate arena");
  }
  ModuleGraph graph;
  if (!module_graph_init(&graph, arena, project_root)) {
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

  Chunk chunk;
  chunk_init(&chunk);
  ok = vm_compile_module(&merged, &chunk, entry_file);
  if (ok) {
    ok = write_vm_chunk_file(&chunk, chunk_path);
  }
  if (ok) {
    ok = write_function_manifest(&merged, chunk_path);
  }
  if (ok) {
    ok = c_backend_emit(&merged, c_path);
  }
  if (ok) {
    ok = copy_runtime_assets(compiled_dir);
  }

  chunk_free(&chunk);
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
  VmRegistry registry;
  vm_registry_init(&registry);
  TickCounter tick_counter = {.next = 0};
  if (!register_default_natives(&registry, &tick_counter)) {
    fprintf(stderr, "error: failed to register VM native functions\n");
    vm_registry_free(&registry);
    chunk_free(&chunk);
    return 1;
  }
  vm_set_registry(&vm, &registry);
  VMResult result = vm_run(&vm, &chunk);
  vm_registry_free(&registry);
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
  bool is_pack = (strcmp(cmd, "pack") == 0);

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
  } else if (is_pack) {
    PackOptions pack_opts;
    if (!parse_pack_args(argc, argv, &pack_opts)) {
      print_usage(argv[-1]);
      return 1;
    }
    return run_raepack_file(&pack_opts);
  } else if (is_build) {
    BuildOptions build_opts;
    if (!parse_build_args(argc, argv, &build_opts)) {
      print_usage(argv[-1]);
      return 1;
    }
    if (!file_exists(build_opts.entry_path)) {
      fprintf(stderr, "error: entry file '%s' not found\n", build_opts.entry_path);
      return 1;
    }
    if (build_opts.project_path && !directory_exists(build_opts.project_path)) {
      fprintf(stderr, "error: project path '%s' not found or not a directory\n",
              build_opts.project_path);
      return 1;
    }
    switch (build_opts.target) {
      case BUILD_TARGET_LIVE:
        return build_vm_output(build_opts.entry_path,
                               build_opts.project_path,
                               build_opts.out_path) ?
                   0 :
                   1;
      case BUILD_TARGET_COMPILED:
        if (!build_opts.emit_c) {
          fprintf(stderr, "error: --emit-c is required for compiled builds\n");
          return 1;
        }
        return build_c_backend_output(build_opts.entry_path,
                                      build_opts.project_path,
                                      build_opts.out_path) ?
                   0 :
                   1;
      case BUILD_TARGET_HYBRID:
        return build_hybrid_output(build_opts.entry_path,
                                   build_opts.project_path,
                                   build_opts.out_path) ?
                   0 :
                   1;
      default:
        fprintf(stderr, "error: unsupported build target\n");
        return 1;
    }
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
       strcmp(cmd, "run") == 0 || strcmp(cmd, "pack") == 0 || strcmp(cmd, "build") == 0)) {
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
  TickCounter tick_counter = {.next = 0};
  if (!register_default_natives(&registry, &tick_counter)) {
    fprintf(stderr, "error: failed to register VM native functions\n");
    vm_registry_free(&registry);
    return 1;
  }
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
          vm_set_registry(&vm, &registry);
          reload_count += 1;
          printf("[watch] running latest version... (reload #%d)\n", reload_count);
          fflush(stdout);
          tick_counter.next = 0;
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
