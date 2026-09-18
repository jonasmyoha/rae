/* main.c - Rae compiler entry point */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>  /* _NSGetExecutablePath — for compiler-relative stdlib */
#endif
#ifndef RAE_RUNTIME_SOURCE_DIR
#define RAE_RUNTIME_SOURCE_DIR "runtime"
#endif

/* C emission retains parsed AST and generated specializations together. Large
 * applications can legitimately exceed a smaller budget by adding a single
 * feature module (e.g. a full water/ocean subsystem on top of an already-large
 * renderer client tips it over): the substitute_type_ref specialization nodes
 * for one more heavily-generic module graph run to tens of MiB. Raised 128 -> 512
 * MiB so production-sized module graphs have real headroom; it is a one-shot
 * malloc freed after emit, and only touched pages become resident. */
#define RAE_C_BACKEND_ARENA_CAPACITY (512ULL * 1024ULL * 1024ULL)
#include "arena.h"
#include "str.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "pretty.h"
#include "rae_format.h"
#include "rae_version.h"
#include "rae_deps.h"
#include "../build/version_gen.h"
#include "c_backend.h"
#include "sema.h"
#include "mangler.h"
#include "bindgen.h"
#include "raepack.h"
#include "raepack.h"
#include "sys_thread.h"
#include "progress.h"
#include "../runtime/rae_runtime.h"

typedef struct {
  const char* input_path;
  const char* project_path;
  bool watch;
  int timeout;
  int target;
  int profile;
  bool no_implicit;
  bool zero_config;  // entry was inferred from the cwd (folder `rae run`/`watch`)
  // #995: everything after the first `--`, handed verbatim to the built app
  // (argv[1..] of the child). NULL/0 when there is no `--`.
  int app_argc;
  char** app_argv;
} RunOptions;

typedef struct {
  const char* entry_path;
  const char* out_path;
  const char* project_path;
  bool emit_c;
  int target;
  int profile;
  bool no_implicit;
} BuildOptions;

typedef struct {
  const char* file_path;
  const char* target_id;
  bool json;
} PackOptions;

typedef enum {
  BUILD_TARGET_COMPILED = 0,
  BUILD_TARGET_WASM
} BuildTarget;

typedef enum {
  BUILD_PROFILE_RELEASE = 0,
  BUILD_PROFILE_DEV
} BuildProfile;


typedef struct ModuleNode {
  char* module_path;
  char* file_path;
  char* canonical_path;
  AstModule* module;
  bool is_auto_loaded;   // intentional stdlib prelude (core/string/math/io/sys) — implicitly opened everywhere; NOT incidentally-discovered deps (docs/module-namespacing.md)
  struct ModuleNode* next;
} ModuleNode;

typedef struct {
  ModuleNode* head;
  ModuleNode* tail;
  char* root_path;
  // True when root_path came from an EXPLICIT `--project` (so it is the real
  // project source root and safe to scan wholesale). False when root_path was
  // discovered by ascending to a `lib/core.rae` marker, which may sit far above
  // the project — in that case the implicit scan uses the entry file's own
  // directory instead. (docs/module-namespacing.md)
  bool explicit_root;
  Arena* arena;
} ModuleGraph;

/* ---- Build timing (devtools "Run all", perf tracking) -------------------
 *
 * Every compiled build prints ONE machine-readable line to stderr when the C
 * compiler finishes:
 *
 *   @@RAE_BUILD_TIME@@ entry=<path> total_ms=<n> emit_ms=<n> cc_ms=<n>
 *       lines=<n> project_lines=<n> modules=<n> ms_per_kloc=<n.n>
 *
 * emit_ms is the Rae front end + C emission (parse, sema, codegen over the
 * whole module graph); cc_ms is the system C compiler + link, which dominates
 * and scales with -O. `lines` counts every source line the build actually
 * processed (project + stdlib + deps) — the honest denominator for a
 * "ms per 1,000 lines" figure that survives an example growing;
 * `project_lines` is the subset under the project root, for the reader who
 * wants to know how big the app itself is. `rae run` adds two more lines
 * around the app itself, so a driver can budget the build and the run
 * separately:
 *
 *   @@RAE_APP_START@@ entry=<path>
 *   @@RAE_APP_EXIT@@ entry=<path> code=<n> run_ms=<n>
 *
 * With RAE_PROGRESS=lines the build also streams its progress estimate
 * (progress.h): `@@RAE_BUILD_PROGRESS@@ phase=<k> fraction=<f> elapsed_ms=<n>`.
 *
 * They go to stderr, unbuffered, so they never interleave with the program's
 * own stdout; the test runner strips the `@@RAE_` lines before comparing. */
static long long g_build_total_lines = 0;
static long long g_build_project_lines = 0;
static long long g_build_modules = 0;
static long long g_build_emit_ms = 0;

static long long rae_now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000 + (long long)tv.tv_usec / 1000;
}

static long long count_source_lines(const char* source, size_t len) {
  long long lines = 0;
  for (size_t i = 0; i < len; i++) if (source[i] == '\n') lines++;
  if (len > 0 && source[len - 1] != '\n') lines++;
  return lines;
}

static void build_timing_reset(void) {
  g_build_total_lines = 0;
  g_build_project_lines = 0;
  g_build_modules = 0;
  g_build_emit_ms = 0;
}

static void build_timing_print(const char* entry, long long cc_ms) {
  long long total_ms = g_build_emit_ms + cc_ms;
  double per_kloc = g_build_total_lines > 0
      ? (double)total_ms * 1000.0 / (double)g_build_total_lines
      : 0.0;
  fprintf(stderr,
          "@@RAE_BUILD_TIME@@ entry=%s total_ms=%lld emit_ms=%lld cc_ms=%lld lines=%lld "
          "project_lines=%lld modules=%lld ms_per_kloc=%.1f\n",
          entry, total_ms, g_build_emit_ms, cc_ms, g_build_total_lines,
          g_build_project_lines, g_build_modules, per_kloc);
  fflush(stderr);
}

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
static bool build_c_backend_output(const char* entry_file,
                                   const char* project_root,
                                   const char* out_file,
                                   bool no_implicit,
                                   bool* out_uses_sdl3,
                                   bool* out_uses_webgpu,
                                   WatchSources* out_sources,
                                   ProgressPhase progress_last);
static void watch_channel_id(const char* project_root, const char* entry,
                             char* out, size_t out_size);
static bool ensure_directory_p(const char* path);
static bool ensure_directory_tree(const char* dir_path);
static bool ensure_parent_directory(const char* file_path);
static bool copy_runtime_assets(const char* dest_dir);
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
static const char* watch_state_poll_change(WatchState* state);
static void watch_state_absorb_formatted(WatchState* state, const char* build_dir);
static int run_compiled_file(const RunOptions* run_opts, const char* project_root);


static bool file_exists(const char* path);  // defined below

// Zero-config entry inference for `rae run` / `rae watch` with no file arg:
// extract the "entry" string from a devtools.json manifest in the cwd. Not a
// full JSON parser — finds the first "entry" key + its quoted value, which is
// all the well-formed example manifests need. Returns a static buffer or NULL.
static const char* manifest_entry_in_cwd(void) {
  FILE* f = fopen("devtools.json", "rb");
  if (!f) return NULL;
  static char buf[8192];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  char* k = strstr(buf, "\"entry\"");
  if (!k) return NULL;
  char* colon = strchr(k + 7, ':'); if (!colon) return NULL;
  char* q1 = strchr(colon, '"'); if (!q1) return NULL;
  q1 += 1;
  char* q2 = strchr(q1, '"'); if (!q2) return NULL;
  size_t len = (size_t)(q2 - q1);
  static char entry[PATH_MAX];
  if (len == 0 || len >= sizeof(entry)) return NULL;
  memcpy(entry, q1, len); entry[len] = '\0';
  return entry;
}

static bool parse_run_args(int argc, char** argv, RunOptions* opts) {
  opts->watch = false;
  opts->input_path = NULL;
  opts->project_path = NULL;
  opts->timeout = 0;
  // The Live (bytecode VM) target is frozen/deprecated, so `rae run` / `rae
  // watch` default to Compiled (C backend). Live only runs on an explicit
  // `--target live` (which warns). Previously the default was Live and only the
  // zero-config path below flipped it to Compiled, so passing an explicit file
  // (`rae watch main.rae`) silently used the frozen VM.
  opts->target = BUILD_TARGET_COMPILED;
  opts->profile = BUILD_PROFILE_RELEASE;
  opts->no_implicit = false;
  opts->zero_config = false;
  opts->app_argc = 0;
  opts->app_argv = NULL;

  int i = 0;
  while (i < argc) {
    const char* arg = argv[i];
    if (strcmp(arg, "--") == 0) {
      /* #995: the rest belongs to the program, not to `rae run`. */
      opts->app_argc = argc - (i + 1);
      opts->app_argv = argv + (i + 1);
      break;
    }
    if (strcmp(arg, "--no-implicit") == 0) {
      opts->no_implicit = true;
      i += 1;
      continue;
    }
    if (strcmp(arg, "--check-format") == 0) {
      /* #919: refuse to rewrite non-canonical sources; fail with the list. */
      rae_preflight_set_mode(RAE_PREFLIGHT_CHECK);
      i += 1;
      continue;
    }
    if (strcmp(arg, "--check-toolchain") == 0) {
      /* #931: consumed by toolchain_check (which rescans argv); recognised
       * here so the arg parser does not reject it as unknown. */
      i += 1;
      continue;
    }
    // Build profile for the compiled target: release (-O2 -DNDEBUG) or
    // dev/debug (-O0 -g). Ignored by the live (bytecode) target. The
    // `--release` / `--debug` aliases mirror the common convention.
    if (strcmp(arg, "--release") == 0) {
      opts->profile = BUILD_PROFILE_RELEASE;
      i += 1;
      continue;
    }
    if (strcmp(arg, "--debug") == 0) {
      opts->profile = BUILD_PROFILE_DEV;
      i += 1;
      continue;
    }
    if (strcmp(arg, "--profile") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --profile expects dev or release\n");
        return false;
      }
      const char* value = argv[i + 1];
      if (strcmp(value, "dev") == 0 || strcmp(value, "debug") == 0) {
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
    if (strcmp(arg, "--watch") == 0 || strcmp(arg, "-w") == 0) {
      opts->watch = true;
      i += 1;
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
    if (strcmp(arg, "--timeout") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --timeout expects an integer value in seconds\n");
        return false;
      }
      opts->timeout = atoi(argv[i+1]);
      i += 2;
      continue;
    }
    if (strcmp(arg, "--target") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --target expects a target name (compiled)\n");
        return false;
      }
      if (strcmp(argv[i+1], "compiled") == 0) {
        opts->target = BUILD_TARGET_COMPILED;
      } else {
        fprintf(stderr, "error: unknown target '%s' for run (only 'compiled' is supported)\n", argv[i+1]);
        return false;
      }
      i += 2;
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
    // Zero-config: infer the entry from the current directory so `rae run` /
    // `rae watch` work from inside a project/example folder (cargo-style). Use
    // main.rae, else the devtools.json manifest's "entry". The target already
    // defaults to Compiled above (Live is frozen); an explicit --target wins.
    const char* entry = NULL;
    // `main.rae` is the `Main` module (module files are PascalCase), so prefer
    // `Main.rae`; keep accepting a lowercase `main.rae` so un-migrated projects
    // still run (#803).
    if (file_exists("Main.rae")) {
      entry = "Main.rae";
    } else if (file_exists("main.rae")) {
      entry = "main.rae";
    } else {
      const char* me = manifest_entry_in_cwd();
      if (me && file_exists(me)) entry = me;
    }
    if (entry) {
      // The cwd (the project/example folder) is the module root, so bare
      // sibling imports (e.g. 53's `import scene`) resolve against it — exactly
      // like the devtools' `--project {{ENTRY_DIR}}`. The entry is made
      // absolute so it's independent of the exec cwd. stdlib `lib/` + assets
      // resolve from the repo root, which run_compiled_file chdir's into.
      static char abs_entry[PATH_MAX];
      opts->input_path = realpath(entry, abs_entry) ? abs_entry : entry;
      opts->zero_config = true;
      if (!opts->project_path) {
        static char proj_dir[PATH_MAX];
        if (realpath(".", proj_dir)) opts->project_path = proj_dir;
      }
    }
  }

  if (!opts->input_path) {
    fprintf(stderr, "error: run requires a file argument, or a Main.rae / "
                    "devtools.json in the current directory\n");
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
  opts->no_implicit = false;

  const char* entry_from_flag = NULL;
  const char* entry_positional = NULL;

  int i = 0;
  while (i < argc) {
    const char* arg = argv[i];
    if (strcmp(arg, "--no-implicit") == 0) {
      opts->no_implicit = true;
      i += 1;
      continue;
    }
    if (strcmp(arg, "--check-format") == 0) {
      /* #919: refuse to rewrite non-canonical sources; fail with the list. */
      rae_preflight_set_mode(RAE_PREFLIGHT_CHECK);
      i += 1;
      continue;
    }
    if (strcmp(arg, "--check-toolchain") == 0) {
      /* #931: consumed by toolchain_check (which rescans argv); recognised
       * here so the arg parser does not reject it as unknown. */
      i += 1;
      continue;
    }
    if (strcmp(arg, "--emit-c") == 0) {
      opts->emit_c = true;
      i += 1;
      continue;
    }
    if (strcmp(arg, "--target") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --target expects one of compiled|wasm\n");
        return false;
      }
      const char* value = argv[i + 1];
      if (strcmp(value, "compiled") == 0) {
        opts->target = BUILD_TARGET_COMPILED;
      } else if (strcmp(value, "wasm") == 0) {
        opts->target = BUILD_TARGET_WASM;
      } else {
        fprintf(stderr, "error: unknown target '%s' (expected compiled|wasm)\n", value);
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
      case BUILD_TARGET_WASM:
        opts->out_path = "build/web/index.html";
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

static bool copy_runtime_assets(const char* out_dir) {
  char src_h[PATH_MAX];
  char src_c[PATH_MAX];
  char dst_h[PATH_MAX];
  char dst_c[PATH_MAX];

  snprintf(src_h, sizeof(src_h), "%s/rae_runtime.h", RAE_RUNTIME_SOURCE_DIR);
  snprintf(src_c, sizeof(src_c), "%s/rae_runtime.c", RAE_RUNTIME_SOURCE_DIR);
  snprintf(dst_h, sizeof(dst_h), "%s/rae_runtime.h", out_dir);
  snprintf(dst_c, sizeof(dst_c), "%s/rae_runtime.c", out_dir);

  if (!copy_file_to(src_h, dst_h)) return false;
  if (!copy_file_to(src_c, dst_c)) return false;

  DIR* runtime_dir = opendir(RAE_RUNTIME_SOURCE_DIR);
  if (!runtime_dir) {
    fprintf(stderr, "error: could not open runtime source dir '%s': %s\n", RAE_RUNTIME_SOURCE_DIR, strerror(errno));
    return false;
  }
  struct dirent* ent;
  while ((ent = readdir(runtime_dir)) != NULL) {
    const char* name = ent->d_name;
    size_t len = strlen(name);
    // Both .c and .h: runtime modules may share a header (runtime_sky_wgsl.h
    // holds the sky shader source used by the forward and deferred paths), and
    // an emitted project that got the .c files but not their headers fails to
    // compile with a missing-include error that points at the copy step rather
    // than at anything the user wrote.
    bool is_c = (len >= 3 && strcmp(name + len - 2, ".c") == 0);
    bool is_h = (len >= 3 && strcmp(name + len - 2, ".h") == 0);
    if (strncmp(name, "runtime_", 8) != 0 || (!is_c && !is_h)) {
      continue;
    }
    char mod_src[PATH_MAX];
    char mod_dst[PATH_MAX];
    snprintf(mod_src, sizeof(mod_src), "%s/%s", RAE_RUNTIME_SOURCE_DIR, name);
    snprintf(mod_dst, sizeof(mod_dst), "%s/%s", out_dir, name);
    if (!copy_file_to(mod_src, mod_dst)) {
      closedir(runtime_dir);
      return false;
    }
  }
  closedir(runtime_dir);

  // rae_runtime.c #includes the vendored lodepng (PNG codec, Rae Image API),
  // so the emitted standalone project needs both files alongside it.
  char lp_src[PATH_MAX];
  char lp_dst[PATH_MAX];
  snprintf(lp_src, sizeof(lp_src), "%s/lodepng.h", RAE_RUNTIME_SOURCE_DIR);
  snprintf(lp_dst, sizeof(lp_dst), "%s/lodepng.h", out_dir);
  if (!copy_file_to(lp_src, lp_dst)) return false;
  snprintf(lp_src, sizeof(lp_src), "%s/lodepng.c", RAE_RUNTIME_SOURCE_DIR);
  snprintf(lp_dst, sizeof(lp_dst), "%s/lodepng.c", out_dir);
  if (!copy_file_to(lp_src, lp_dst)) return false;
  // Vendored stb_image (JPEG decode for gpu2d, #228 / design #217) —
  // included by rae_runtime.c the same way as lodepng.
  snprintf(lp_src, sizeof(lp_src), "%s/stb_image.h", RAE_RUNTIME_SOURCE_DIR);
  snprintf(lp_dst, sizeof(lp_dst), "%s/stb_image.h", out_dir);
  return copy_file_to(lp_src, lp_dst);
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
  node->module_path = module_path ? strdup(module_path) : NULL;
  node->file_path = strdup(file_path);
  
  char canonical[PATH_MAX];
  if (realpath(file_path, canonical)) {
      node->canonical_path = strdup(canonical);
  } else {
      node->canonical_path = strdup(file_path);
  }

  node->module = module;
  if (module) {
      module->file_path = node->file_path;
  }
  if ((module_path && !node->module_path) || !node->file_path || !node->canonical_path) {
    fprintf(stderr, "error: out of memory while duplicating module paths\n");
    if (node->module_path) free(node->module_path);
    free(node->file_path);
    if (node->canonical_path) free(node->canonical_path);
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

  // #779: strip a trailing `.rae` up front so the `.`->`/` separator conversion
  // below never mangles the extension (derive_module_path passes real file paths
  // like `lib/ui/Theme.rae`). The per-segment strip further down then no-ops.
  {
    size_t szn = strlen(sanitized);
    if (szn > 4 && strcmp(sanitized + szn - 4, ".rae") == 0) sanitized[szn - 4] = '\0';
  }

  SegmentBuffer segments = {0};
  const char* cursor = sanitized;
  bool treat_as_relative = false;
  if (cursor[0] == '/') {
    while (*cursor == '/') cursor++;
  } else {
    treat_as_relative = is_relative_spec(cursor);
  }

  // #779: absolute imports accept `.` OR `/` as the package/module separator, so
  // `import ui.Theme` == `import ui/Theme`. Relative specs (leading `.`/`..`) keep
  // `/` so `../bb` still means the parent package. The internal module_path stays
  // `/`-separated, so sema/mangler are unchanged.
  if (!treat_as_relative) {
    for (char* p = (char*)cursor; *p; ++p) { if (*p == '.') *p = '/'; }
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
  if (!root) {
    size_t mod_len = strlen(module_path);
    size_t total = mod_len + 5;
    char* buffer = malloc(total);
    if (!buffer) return NULL;
    snprintf(buffer, total, "%s.rae", module_path);
    return buffer;
  }
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
  if (!root) {
    return normalize_import_path(NULL, file_path);
  }
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


// Set by the CLI dispatch when the project root came from an explicit
// `--project` (as opposed to an ascended lib/core.rae marker). Process-wide:
// one invocation is either --project'd or not. Read into graph->explicit_root
// so the implicit scan knows whether root_path is safe to scan wholesale.
static bool s_explicit_project_root = false;

static bool module_graph_init(ModuleGraph* graph, Arena* arena, const char* project_root) {
  memset(graph, 0, sizeof(*graph));
  graph->arena = arena;
  graph->explicit_root = s_explicit_project_root;
  char* resolved = NULL;
  bool root_from_cwd = (project_root == NULL || project_root[0] == '\0');
  if (!root_from_cwd) {
    resolved = realpath(project_root, NULL);
    if (!resolved) {
      fprintf(stderr, "error: unable to resolve project path '%s'\n", project_root);
      return false;
    }
    graph->root_path = resolved;
  } else {
    graph->root_path = NULL; // No strict root for CWD-based runs
  }
  
  if (graph->root_path) {
    size_t len = strlen(graph->root_path);
    while (len > 1 && (graph->root_path[len - 1] == '/' || graph->root_path[len - 1] == '\\')) {
      graph->root_path[len - 1] = '\0';
      len -= 1;
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

// The compiler's own bundled stdlib directory, so any project can use
// the single canonical stdlib without a per-project `lib/` symlink or
// copy — the same way gcc finds its headers and rustc its sysroot.
//
// Resolution order (computed once, cached):
//   1. $RAE_STDLIB (explicit override), if it contains core.rae.
//   2. Relative to the `rae` executable: the binary lives at
//      <repo>/compiler/bin/rae and the stdlib at <repo>/lib, so
//      <exedir>/../../lib. We also probe <exedir>/../lib and
//      <exedir>/lib so a relocated/installed binary still finds it.
// Returns NULL when no directory containing core.rae is found.
static const char* compiler_stdlib_dir(void) {
  static char cached[PATH_MAX];
  static int computed = 0;
  if (computed) {
    return cached[0] ? cached : NULL;
  }
  computed = 1;
  cached[0] = '\0';

  const char* env = getenv("RAE_STDLIB");
  if (env && env[0]) {
    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/core/Core.rae", env);  /* #818: core is a package */
    if (file_exists(probe)) {
      snprintf(cached, sizeof(cached), "%s", env);
      return cached;
    }
  }

  char exe[PATH_MAX];
#ifdef __APPLE__
  uint32_t size = sizeof(exe);
  if (_NSGetExecutablePath(exe, &size) != 0) {
    return NULL;
  }
#elif defined(__linux__)
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) {
    return NULL;
  }
  exe[n] = '\0';
#else
  return NULL;
#endif
  char real[PATH_MAX];
  if (!realpath(exe, real)) {
    snprintf(real, sizeof(real), "%s", exe);
  }
  char* slash = strrchr(real, '/');
  if (!slash) {
    return NULL;
  }
  *slash = '\0';  // `real` is now the executable's directory.

  const char* suffixes[] = { "/../../lib", "/../lib", "/lib", NULL };
  for (int i = 0; suffixes[i]; i++) {
    char cand[PATH_MAX];
    snprintf(cand, sizeof(cand), "%s%s", real, suffixes[i]);
    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/core/Core.rae", cand);  /* #818: core is a package */
    if (file_exists(probe)) {
      if (!realpath(cand, cached)) {
        snprintf(cached, sizeof(cached), "%s", cand);
      }
      return cached;
    }
  }
  cached[0] = '\0';
  return NULL;
}

static char* try_resolve_lib_module(const char* root, const char* normalized) {
  // #933: a declared dependency owns its import prefix (`import codec/Foo`
  // -> <depdir>/Foo.rae) ahead of any lib/ or stdlib fallback.
  {
    char* dep_file = rae_deps_resolve_module_file(normalized);
    if (dep_file) return dep_file;
  }
  // Check root/lib/normalized.rae
  if (root) {
    size_t root_len = strlen(root);
    size_t mod_len = strlen(normalized);
    size_t total = root_len + mod_len + 10; // /lib/ .rae
    char* buffer = malloc(total);
    if (buffer) {
      if (root_len > 0 && (root[root_len - 1] == '/' || root[root_len - 1] == '\\')) {
        root_len -= 1;
      }
      snprintf(buffer, total, "%.*s/lib/%s.rae", (int)root_len, root, normalized);
      if (file_exists(buffer)) {
        return buffer;
      }
      free(buffer);
    }
  }

  // Fallback: Check relative to compiler location (e.g. from tests/ or bin/)
  size_t mod_len = strlen(normalized);
  size_t total_fallback = mod_len + 16;
  char* b2 = malloc(total_fallback);
  if (b2) {
      snprintf(b2, total_fallback, "../lib/%s.rae", normalized);
      if (file_exists(b2)) return b2;
      
      snprintf(b2, total_fallback, "lib/%s.rae", normalized);
      if (file_exists(b2)) return b2;

      free(b2);
  }

  // Final fallback: the compiler's own bundled stdlib (relative to the
  // `rae` binary, or $RAE_STDLIB). This is what lets a sibling project
  // like game proto1 use the one stdlib without a `lib/` symlink —
  // it only kicks in when the project doesn't ship its own stdlib.
  const char* stdlib = compiler_stdlib_dir();
  if (stdlib) {
    size_t total = strlen(stdlib) + strlen(normalized) + 8; // "/" + ".rae"
    char* b3 = malloc(total);
    if (b3) {
      snprintf(b3, total, "%s/%s.rae", stdlib, normalized);
      if (file_exists(b3)) return b3;
      free(b3);
    }
  }

  return NULL;
}

// #786: resolve `import math` where `math` is a DIRECTORY (folder-package) rather
// than a `math.rae` file. Mirrors try_resolve_lib_module's search order but checks
// for a directory. Returns a malloc'd path to the folder, or NULL.
static char* try_resolve_folder(const char* root, const char* normalized) {
  {
    char* dep_dir = rae_deps_resolve_folder(normalized);  /* #933 */
    if (dep_dir) return dep_dir;
  }
  if (root) {
    const char* fmts[2] = { "%s/%s", "%s/lib/%s" };
    for (int i = 0; i < 2; i++) {
      size_t total = strlen(root) + strlen(normalized) + 8;
      char* b = malloc(total);
      if (!b) return NULL;
      snprintf(b, total, fmts[i], root, normalized);
      if (directory_exists(b)) return b;
      free(b);
    }
  }
  size_t nl = strlen(normalized);
  char* b2 = malloc(nl + 16);
  if (b2) {
    snprintf(b2, nl + 16, "../lib/%s", normalized); if (directory_exists(b2)) return b2;
    snprintf(b2, nl + 16, "lib/%s", normalized); if (directory_exists(b2)) return b2;
    free(b2);
  }
  const char* stdlib = compiler_stdlib_dir();
  if (stdlib) {
    size_t total = strlen(stdlib) + nl + 4;
    char* b3 = malloc(total);
    if (b3) { snprintf(b3, total, "%s/%s", stdlib, normalized); if (directory_exists(b3)) return b3; free(b3); }
  }
  return NULL;
}

static bool module_graph_load_module(ModuleGraph* graph,
                                     const char* module_path,
                                     const char* file_path,
                                     ModuleStack* stack,
                                     uint64_t* hash_out,
                                     bool no_implicit) {
    char canonical_path[PATH_MAX];
    const char* path_to_check = file_path;
    if (realpath(file_path, canonical_path)) {
      path_to_check = canonical_path;
    }
  
    // Check by canonical path first (most reliable)
    for (ModuleNode* node = graph->head; node; node = node->next) {
      if (node->canonical_path && strcmp(node->canonical_path, path_to_check) == 0) {
        return true;
      }
    }
  
    if (module_graph_has_module(graph, module_path)) {
      return true;
    }
  
  if (module_stack_contains(stack, module_path)) {
    // Cyclic import: this module is already being loaded higher up the stack and
    // WILL be appended to the graph once that frame finishes its imports. Rae
    // compiles by MERGING every loaded module into one AstModule and running one
    // global sema/codegen pass over it (see merge_module_graph), so load order is
    // irrelevant to name/type resolution and mutual references across modules are
    // safe. The recursive loader must only avoid re-entering an in-progress module
    // (which would infinitely recurse); returning true here breaks the recursion
    // and lets the outer frame complete the append. This is what lets two ECS
    // systems reference each other's types without an artificial shared "types"
    // module or dependency inversion (#743).
    return true;
  }
  size_t file_size = 0;
  char* source = read_file(file_path, &file_size);
  if (!source) {
    fprintf(stderr, "error: could not read module file '%s'\n", file_path);
    module_stack_print_trace(stack, module_path);
    return false;
  }
  /* #919: the formatter runs FIRST — the build lexes the canonical bytes. */
  /* #933: dependency sources are vendored/cloned, not this project's — the
   * canonical-format preflight neither rewrites nor check-fails them. */
  if (!rae_deps_owns_path(file_path) && !rae_preflight_source(file_path, &source, &file_size)) {
    free(source);
    return false;
  }
  if (hash_out) {
    uint64_t module_hash = hash_bytes(source, file_size);
    *hash_out ^= module_hash + 0x9e3779b97f4a7c15ull + (*hash_out << 6) + (*hash_out >> 2);
  }
  {
    long long lines = count_source_lines(source, file_size);
    g_build_total_lines += lines;
    g_build_modules++;
    progress_module_loaded((int)g_build_modules);
    if (graph->root_path && strncmp(path_to_check, graph->root_path, strlen(graph->root_path)) == 0) {
      g_build_project_lines += lines;
    }
  }

  // Default mode: every stdlib module (core, string, math, io, sys, char)
  // is auto-loaded unless this file opts out via `import "nostdlib"` or
  // the compiler was invoked with `--no-implicit`. The old trigger-
  // symbol scanner (needs_math, needs_string, …) was a brittle
  // hand-picked allowlist — e.g. it caught `.concat()` and `.sub()` but
  // not `.length()` or `.equals()`, forcing every caller of those to
  // write an explicit `import string`. Dead-code elimination at the C
  // backend / VM mangler level handles the unused-symbol cost.
  bool use_stdlib = true;
  {
    TokenList tokens = lexer_tokenize(graph->arena, file_path, source, file_size, true);
    if (tokens.had_error) {
        free(source);
        return false;
    }
    for (size_t i = 0; i < tokens.count; i++) {
        Token* t = &tokens.data[i];
        if (t->kind == TOK_KW_IMPORT || t->kind == TOK_KW_EXPORT) {
            if (i + 1 < tokens.count) {
                Token* next = &tokens.data[i+1];
                if (next->kind == TOK_IDENT && str_eq_cstr(next->lexeme, "nostdlib")) {
                    use_stdlib = false;
                }
                if (next->kind == TOK_STRING || next->kind == TOK_STRING_START || next->kind == TOK_RAW_STRING) {
                    Str s = next->lexeme;
                    if (next->kind == TOK_STRING || next->kind == TOK_STRING_START) {
                        if (s.len >= 10 && strncmp(s.data + 1, "nostdlib", 8) == 0 && (s.data[9] == '"' || s.data[9] == ' ' || s.data[9] == '\0' || s.data[9] == '}')) {
                            use_stdlib = false;
                        }
                    } else {
                        if (s.len == 8 && strncmp(s.data, "nostdlib", 8) == 0) {
                            use_stdlib = false;
                        }
                    }
                }
            }
        }
    }
  }

  ModuleStack frame = {.module_path = module_path, .next = stack};
  // Auto-load every stdlib module unless this file is itself one of them
  // (the self-exclude list below prevents cycles — e.g. lib/string.rae
  // depends on lib/core.rae but lib/core.rae must NOT auto-load
  // lib/string.rae). All other files get the full stdlib for free.
  if (use_stdlib && !no_implicit &&
      (!module_path || (strncmp(module_path, "core/", 5) != 0 &&
                        strcmp(module_path, "Math") != 0 &&
                        strcmp(module_path, "Io") != 0 &&
                        strcmp(module_path, "String") != 0 &&
                        strcmp(module_path, "Sys") != 0))) {
    // Prelude: auto-loaded => auto-opened for every file. `core/Core` pulls in
    // its sibling `core/List` (the generic List(T)), which is a fundamental type
    // used pervasively via bare/UFCS (.add/.get) — requiring a per-file
    // `open core/List` would be impractical and break "list operations keep
    // working" (docs/module-namespacing.md).
    static const char* stdlib_modules[] = { "core/Core", "String", "Math", "Io", "Sys" };
    for (size_t i = 0; i < sizeof(stdlib_modules) / sizeof(stdlib_modules[0]); i++) {
      const char* name = stdlib_modules[i];
      if (module_graph_has_module(graph, name)) continue;
      if (module_stack_contains(&frame, name)) continue;
      char* path = try_resolve_lib_module(graph->root_path, name);
      if (!path) continue;
      if (!module_graph_load_module(graph, name, path, &frame, hash_out, no_implicit)) {
        free(path);
        return false;
      }
      free(path);
      // Mark the intentional prelude so resolution treats it as opened
      // everywhere — distinct from modules incidentally discovered via imports.
      ModuleNode* loaded = module_graph_find(graph, name);
      if (loaded) loaded->is_auto_loaded = true;
      // #818: a package prelude entry (`core/Core`) auto-loads its siblings via
      // the isMain rule; auto-OPEN the whole folder so every sibling's decls
      // (List, the maps) are bare-callable everywhere, exactly as the single
      // core.rae was before the split.
      const char* pkgslash = strrchr(name, '/');
      if (pkgslash) {
        size_t folder_len = (size_t)(pkgslash - name) + 1;  // include the '/'
        for (ModuleNode* n = graph->head; n; n = n->next) {
          if (n->module_path && strncmp(n->module_path, name, folder_len) == 0)
            n->is_auto_loaded = true;
        }
      }
    }
  }

  TokenList tokens = lexer_tokenize(graph->arena, file_path, source, file_size, true);
  if (tokens.had_error) {
      free(source);
      return false;
  }
  AstModule* module = parse_module(graph->arena, file_path, tokens);
  if (!module || module->had_error) {
    free(source);
    return false;
  }

  // #787: if this file is a folder-package MAIN module (its basename == PascalCase
  // of its folder's last component, e.g. renderSystem/RenderSystem), also load the
  // sibling .rae files in the same folder, so `import pkg.Main` exposes the whole
  // package. Exporting siblings merge into this main module; the rest become their
  // own modules. A plain module (not named after its folder) does not pull siblings.
  if (module_path) {
    const char* mp = module_path;
    const char* lastSlash = strrchr(mp, '/');
    if (lastSlash && lastSlash > mp) {
      const char* base = lastSlash + 1;
      const char* folderStart = mp;
      for (const char* p = mp; p < lastSlash; p++) if (*p == '/') folderStart = p + 1;
      size_t compLen = (size_t)(lastSlash - folderStart);
      bool isMain = compLen > 0 && strlen(base) == compLen
        && (char)toupper((unsigned char)folderStart[0]) == base[0]
        && (compLen == 1 || strncmp(folderStart + 1, base + 1, compLen - 1) == 0);
      if (isMain) {
        size_t folderLen = (size_t)(lastSlash - mp);
        char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", file_path);
        char* ds = strrchr(dir, '/'); if (ds) *ds = '\0'; else { dir[0] = '.'; dir[1] = '\0'; }
        char thisname[520]; snprintf(thisname, sizeof thisname, "%s.rae", base);
        DIR* dh = opendir(dir);
        if (dh) {
          struct dirent* ent;
          while ((ent = readdir(dh)) != NULL) {
            const char* nm = ent->d_name;
            size_t nlen = strlen(nm);
            if (nlen <= 4 || strcmp(nm + nlen - 4, ".rae") != 0) continue;
            if (strcmp(nm, thisname) == 0) continue;  // skip the main file itself
            char childmod[1024]; char childfile[PATH_MAX];
            snprintf(childmod, sizeof childmod, "%.*s/%.*s", (int)folderLen, mp, (int)(nlen - 4), nm);
            snprintf(childfile, sizeof childfile, "%s/%s", dir, nm);
            module_graph_load_module(graph, childmod, childfile, &frame, NULL, no_implicit);
          }
          closedir(dh);
        }
      }
    }
  }

  for (AstImport* import = module->imports; import; import = import->next) {
    char* raw = str_to_cstr(import->path);
    if (!raw) {
      fprintf(stderr, "error: out of memory while reading import path\n");
      return false;
    }
    if (strcmp(raw, "nostdlib") == 0) {
      free(raw);
      continue;
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
        // #786: folder-package import — `import math` where `math/` is a directory
        // (no `math.rae`) loads every module in it (math/Vec3, math/Trig, ...). They
        // become reachable as `math.Vec3.func()` (the `import math` node registers
        // package `math` for visibility). A file `math.rae` takes precedence above.
        char* folder = try_resolve_folder(graph->root_path, normalized);
        if (folder) {
          DIR* dh = opendir(folder);
          if (dh) {
            struct dirent* ent;
            while ((ent = readdir(dh)) != NULL) {
              const char* nm = ent->d_name;
              size_t nlen = strlen(nm);
              if (nlen > 4 && strcmp(nm + nlen - 4, ".rae") == 0) {
                char childmod[1024];
                char childfile[PATH_MAX];
                snprintf(childmod, sizeof childmod, "%s/%.*s", normalized, (int)(nlen - 4), nm);
                snprintf(childfile, sizeof childfile, "%s/%s", folder, nm);
                module_graph_load_module(graph, childmod, childfile, &frame, NULL, no_implicit);
              }
            }
            closedir(dh);
          }
          free(folder);
          free(normalized);
          free(child_file);
          continue;
        }
        fprintf(stderr, "error: imported module '%s' not found (required by '%s')\n", normalized,
                module_path ? module_path : "<entry>");
        module_stack_print_trace(&frame, normalized);
        free(normalized);
        free(child_file);
        return false;
      }
    }
    if (!module_graph_load_module(graph, normalized, child_file, &frame, hash_out, no_implicit)) {
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
                                       uint64_t* hash_out,
                                       bool no_implicit) {
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
      // #665: never auto-scan a `lib/` directory. `lib` is the external/stdlib
      // PACKAGE space (sema keys visibility on `/lib/` anywhere in the path),
      // reached only through `import`/prelude, which resolve + de-duplicate each
      // module once. A downstream project that symlinks this repo's `lib/` under
      // its own `--project` root would otherwise have the project scan (a) compile
      // un-imported sibling packages (e.g. ui/legacy_raylib) and (b) re-include a
      // module already loaded by import under a second module path -> `redefinition`
      // C errors. Skipping `lib` keeps the scan to the project's OWN source.
      if (strcmp(entry->d_name, "lib") == 0) {
        continue;
      }
      // #933: a dot-directory is never project source (`.git`, `build` caches,
      // and `.rae/deps/<name>` — cloned dependencies, reachable only through the
      // dep resolver under their own import prefix, never as `.rae/deps/...`).
      if (entry->d_name[0] == '.') {
        continue;
      }
      if (!scan_directory_for_modules(graph, child_path, skip_file, hash_out, no_implicit)) {
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
      if (!module_graph_load_module(graph, module_path, child_path, NULL, hash_out, no_implicit)) {
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

static bool auto_import_directory(ModuleGraph* graph, const char* entry_file_path, uint64_t* hash_out, bool no_implicit) {
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
  bool ok = scan_directory_for_modules(graph, dir_copy, entry_file_path, hash_out, no_implicit);
  free(dir_copy);
  return ok;
}

static bool module_graph_build(ModuleGraph* graph, const char* entry_file, uint64_t* hash_out, bool no_implicit) {
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
  bool ok = module_graph_load_module(graph, module_path, resolved_entry, NULL, hash_out, no_implicit);
  if (!ok) {
    free(module_path);
    free(resolved_entry);
    return false;
  }

  // Load core library explicitly if it exists in the project root
  if (graph->root_path && !no_implicit) {
      char core_path[PATH_MAX];
      snprintf(core_path, sizeof(core_path), "%s/lib/core/Core.rae", graph->root_path);  /* #818 */
      if (file_exists(core_path)) {
          // Don't realpath() here: when <project>/lib/ is a symlink
          // into a sibling stdlib checkout (the rae-port pattern), the
          // realpath resolves outside graph->root_path and
          // derive_module_path then rejects it as "outside project
          // root". The unresolved <project>/lib/core.rae path always
          // sits inside root, which is what we need for the relative
          // module-name derivation.
          char* core_module_path = derive_module_path(graph->root_path, core_path);
          if (core_module_path) {
              module_graph_load_module(graph, core_module_path, core_path, NULL, hash_out, no_implicit);
              free(core_module_path);
          }
      }
  }

  ModuleNode* entry_node = module_graph_find(graph, module_path);
  // Implicitly import the project's .rae files (recursively). Scan from the
  // PROJECT ROOT when it was set EXPLICITLY (`--project`), so an entry deep in
  // the tree (game/ui/hud.rae) still sees every project file (game/**). Without
  // an explicit --project, `root_path` is a lib-marker root discovered by
  // ascending to a `lib/core.rae` (often a repo root far above the project), so
  // scanning it would pull in unrelated files — fall back to the entry file's
  // own directory, exactly as before. (docs/module-namespacing.md)
  if (entry_node && !no_implicit) {
    bool scanned = (graph->explicit_root && graph->root_path)
      ? scan_directory_for_modules(graph, graph->root_path, entry_node->file_path, hash_out, no_implicit)
      : auto_import_directory(graph, entry_node->file_path, hash_out, no_implicit);
    if (!scanned) {
      free(module_path);
      free(resolved_entry);
      return false;
    }
  }
  free(module_path);
  free(resolved_entry);
  /* #919: --check-format / RAE_FORMAT=check — the closure is loaded, now
   * refuse the build if any of it was not canonical. */
  if (rae_preflight_report() > 0) return false;
  return true;
}

static AstModule merge_module_graph(const ModuleGraph* graph) {
  AstModule merged = {.file_path = NULL, .imports = NULL, .decls = NULL};
  if (graph->tail && graph->tail->module) {
      merged.file_path = graph->tail->module->file_path;
  }
  AstDecl* head = NULL;
  AstDecl* tail = NULL;
  // Per-file import/open directives are flattened away by the merge; register
  // them keyed by file first so sema can resolve aliases + import/open per file
  // (docs/module-namespacing.md).
  sema_reset_file_scopes();
  for (ModuleNode* node = graph->head; node; node = node->next) {
    if (node->module) sema_register_file_imports(graph->arena, node->module->file_path, node->module->imports);
    // Prelude (auto-load set) is opened for every file. Driven by the load
    // mechanism's flag, not a hardcoded resolver list.
    if (node->is_auto_loaded && node->module_path) sema_register_global_open(graph->arena, node->module_path);
  }
  // Collision guard: namespaced extern symbols are rae_ext_<module-path>_<name>
  // with '/'->'_'. Two DISTINCT module paths must never map to the same prefix
  // (e.g. a hypothetical `map/int` vs top-level `map_int`), or their externs
  // would silently bind to the same C symbol. Reject that at build time so the
  // encoding is provably collision-safe (docs/module-namespacing.md).
  for (ModuleNode* a = graph->head; a; a = a->next) {
    if (!a->module_path) continue;
    char pa[512]; rae_mangle_module_path(a->module_path, pa, sizeof(pa));
    for (ModuleNode* b = a->next; b; b = b->next) {
      if (!b->module_path || strcmp(a->module_path, b->module_path) == 0) continue;
      char pb[512]; rae_mangle_module_path(b->module_path, pb, sizeof(pb));
      if (strcmp(pa, pb) == 0) {
        fprintf(stderr,
          "error: module paths '%s' and '%s' both mangle to the C-symbol prefix "
          "'rae_ext_%s_*' — rename one so namespaced externs cannot collide\n",
          a->module_path, b->module_path, pa);
        merged.had_error = true;
        return merged;
      }
    }
  }
  for (ModuleNode* node = graph->head; node; node = node->next) {
    // #787: a file with a bare `export` directive belongs to the folder-package's
    // same-named MAIN module: its decls' logical module_name is retargeted from
    // `pkg/Helper` to `pkg/PascalCase(pkg)` (e.g. renderSystem/Helper -> renderSystem/
    // RenderSystem), so importing/opening the main module exposes them too.
    const char* target_module_name = node->module_path;
    if (node->module->export_to_main && node->module_path) {
      const char* mp = node->module_path;
      const char* lastSlash = strrchr(mp, '/');
      if (lastSlash && lastSlash > mp) {
        size_t folderLen = (size_t)(lastSlash - mp);
        const char* folderStart = mp;
        for (const char* p = mp; p < lastSlash; p++) if (*p == '/') folderStart = p + 1;
        size_t compLen = (size_t)(lastSlash - folderStart);
        char* built = malloc(folderLen + 2 + compLen);
        if (built) {
          memcpy(built, mp, folderLen);
          built[folderLen] = '/';
          memcpy(built + folderLen + 1, folderStart, compLen);
          built[folderLen + 1] = (char)toupper((unsigned char)folderStart[0]);
          built[folderLen + 1 + compLen] = '\0';
          target_module_name = built;  // persists for the compile (freed at process exit)
        }
      }
    }
    // Stamp each decl with its origin file so sema can answer "did
    // this call come from stdlib?" after the merge — module->file_path
    // alone only remembers the LAST file, not per-decl provenance.
    for (AstDecl* d = node->module->decls; d; d = d->next) {
      if (!d->origin_file) d->origin_file = node->module->file_path;
      // Remember the logical module (e.g. "math", "filesystem") so sema can
      // resolve namespace-qualified calls like `math.sin(x)` after the merge
      // flattens everything (docs/module-namespacing.md).
      if (!d->module_name) d->module_name = target_module_name;
      // Mirror onto the func decl so the mangler (which only sees AstFuncDecl)
      // can build namespace-qualified extern C symbols.
      if (d->kind == AST_DECL_FUNC && !d->as.func_decl.module_name) {
        d->as.func_decl.module_name = target_module_name;
        d->as.func_decl.origin_file = d->origin_file;
      }
    }
    // Copy the declaration list head
    AstDecl* current = node->module->decls;
    if (!head) {
      head = current;
    } else {
      tail->next = current;
    }

    // Find the new tail
    while (current->next) {
      current = current->next;
    }
    tail = current;
  }
  merged.decls = head;
  // Collect C header directives (`cheader "..."`) from every merged file so the
  // backend emits their #includes once. Flattened like imports; deduped by path
  // (general FFI, #497 — library-agnostic).
  {
    AstCHeader* hdr_head = NULL; AstCHeader* hdr_tail = NULL;
    for (ModuleNode* node = graph->head; node; node = node->next) {
      if (!node->module) continue;
      for (AstCHeader* h = node->module->c_headers; h; h = h->next) {
        bool dup = false;
        for (AstCHeader* e = hdr_head; e; e = e->next) if (str_eq(e->path, h->path)) { dup = true; break; }
        if (dup) continue;
        AstCHeader* copy = arena_alloc(graph->arena, sizeof(AstCHeader));
        copy->path = h->path; copy->next = NULL;
        if (!hdr_head) hdr_head = copy; else hdr_tail->next = copy;
        hdr_tail = copy;
      }
    }
    merged.c_headers = hdr_head;
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
  fprintf(stderr, "  format <files|dirs>\n");
  fprintf(stderr, "                  Canonically format Rae source, in place by default.\n");
  fprintf(stderr, "                  Options: --check (list unformatted, non-zero exit),\n");
  fprintf(stderr, "                  --stdout, --write/-w, --stdin, --json, --rules --json.\n");
  fprintf(stderr, "  run [opts] [file] [-- <args...>]\n");
  fprintf(stderr, "                  Build and run Rae source. Everything after the first\n");
  fprintf(stderr, "                  `--` is passed verbatim to the program (Sys.argCount /\n");
  fprintf(stderr, "                  Sys.argAt); `rae watch` forwards it on every restart.\n");
  fprintf(stderr, "                  With no file, infers\n");
  fprintf(stderr, "                  the entry from the current folder (Main.rae, or a\n");
  fprintf(stderr, "                  devtools.json 'entry') and defaults to --target compiled.\n");
  fprintf(stderr, "                  Options: --project <dir>, --watch, --check-format\n");
  fprintf(stderr, "                  (refuse to rewrite non-canonical sources; RAE_FORMAT=check|off),\n");
  fprintf(stderr, "                           --check-toolchain (strict: require a tagged Rae\n");
  fprintf(stderr, "                           matching the project's rae.raepack requirement;\n");
  fprintf(stderr, "                           RAE_TOOLCHAIN_CHECK=off skips the non-strict check),\n");
  fprintf(stderr, "                           --target compiled,\n");
  fprintf(stderr, "                           --profile <dev|release> (or --debug/--release;\n");
  fprintf(stderr, "                           compiled target: dev=-O0 -g, release=-O2 -DNDEBUG)\n");
  fprintf(stderr, "  pack <file>     Validate and summarize a .raepack file\n");
  fprintf(stderr, "                 (options: --json, --target <id>)\n");
  fprintf(stderr,
          "  build [opts]    Build Rae source (Compiled C, Live, Hybrid, or browser WASM)\n");
  fprintf(stderr,
          "                  Options: --entry <file>, --project <dir>, --out <file>\n");
  fprintf(stderr,
          "                           --target <compiled|wasm>, --profile <dev|release>\n");
  fprintf(stderr,
          "  watch <file>    Compiled hot-reload supervisor. Builds and runs <file>,\n");
  fprintf(stderr,
          "                  rebuilds + restarts on source changes. The running app\n");
  fprintf(stderr,
          "                  can preserve state across reloads via lib/hot_reload.\n");
  fprintf(stderr,
          "  init            Scaffold src/, assets/, src/Main.rae, Makefile in the\n");
  fprintf(stderr,
          "                  current directory and download Roboto-Regular.ttf into\n");
  fprintf(stderr,
          "                  assets/. Idempotent — never overwrites existing files.\n");
  fprintf(stderr, "  toolchain <status|use|list>\n");
  fprintf(stderr, "                  Inspect/switch the Rae toolchain (version check,\n");
  fprintf(stderr, "                  `use <req>` checks out + rebuilds a matching tag)\n");
  fprintf(stderr, "  add <name> --path <dir> | --git <url> [--rev <rev>]\n");
  fprintf(stderr, "                  Add a dependency to the project's .raepack\n");
  fprintf(stderr, "  fetch           Resolve dependencies into .rae/deps from the lockfile\n");
  fprintf(stderr, "  update [<name>] Re-resolve dependencies and rewrite rae.lock\n");
  fprintf(stderr, "  tree            Print the resolved dependency graph\n");
  fprintf(stderr, "  --version, -v   Print the compiler version (--json for tooling)\n");
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

/* #930: build the full compiler version string ("0.1.0", "0.2.0-dev.14",
 * with "+dirty" appended on an uncommitted tree). RAE_* come from the
 * generated build/version_gen.h. */
static void build_compiler_version_string(char* out, size_t out_len) {
  const char* dirty_suffix = RAE_GIT_DIRTY ? "+dirty" : "";
  if (RAE_IS_RELEASE) {
    snprintf(out, out_len, "%s%s", RAE_VERSION_BASE, dirty_suffix);
  } else {
    snprintf(out, out_len, "%s-dev.%d%s", RAE_VERSION_BASE, RAE_COMMITS_SINCE_TAG,
             dirty_suffix);
  }
}

/* #930: `rae --version` / `-v`, plain and `--json`. RAE_* come from the
 * generated build/version_gen.h (VERSION file + git state at build time —
 * see tools/gen-version-header.sh and docs/versioning-and-toolchain.md §1). */
static void print_version(bool json) {
  char version[160];
  build_compiler_version_string(version, sizeof(version));
  if (json) {
    printf("{\n");
    printf("  \"version\": ");
    print_json_string(str_from_cstr(version));
    printf(",\n");
    printf("  \"tag\": ");
    print_json_string(str_from_cstr(RAE_GIT_TAG));
    printf(",\n");
    printf("  \"commit\": ");
    print_json_string(str_from_cstr(RAE_GIT_COMMIT));
    printf(",\n");
    printf("  \"date\": ");
    print_json_string(str_from_cstr(RAE_GIT_DATE));
    printf(",\n");
    printf("  \"dirty\": %s\n", RAE_GIT_DIRTY ? "true" : "false");
    printf("}\n");
  } else {
    printf("rae %s (%s %s)\n", version, RAE_GIT_COMMIT, RAE_GIT_DATE);
  }
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
  if (!raepack_parse_file(opts->file_path, &pack, true)) {
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

static bool build_c_backend_output(const char* entry_file,
                                   const char* project_root,
                                   const char* out_file,
                                   bool no_implicit,
                                   bool* out_uses_sdl3,
                                   bool* out_uses_webgpu,
                                   WatchSources* out_sources,
                                   ProgressPhase progress_last) {
  diag_reset();
  build_timing_reset();
  long long emit_started_ms = rae_now_ms();
  /* The terminal progress display (progress.h) runs from here through
   * `progress_last`; the caller that ends the pipeline ends it
   * (gcc_link_c_to_binary, or the --emit-c build that stops after emission).
   * Every failure exit below ends it first so no error is printed over the
   * bar. Its last-build record lives in the app's own `.rae/apps/<app>/`
   * directory, the one `rae run` and `rae watch` already give each app. */
  {
    char cwd_abs[PATH_MAX];
    if (!getcwd(cwd_abs, sizeof(cwd_abs))) snprintf(cwd_abs, sizeof(cwd_abs), ".");
    char channel_id[128];
    watch_channel_id(project_root, entry_file, channel_id, sizeof(channel_id));
    char record_dir[PATH_MAX];
    snprintf(record_dir, sizeof(record_dir), "%s/.rae/apps/%s", cwd_abs, channel_id);
    /* The channel is named after the project root, which is the cwd for a
     * plain `rae run <file>` — so the file name also carries a hash of the
     * entry path, or every example run from the repo root would share one
     * estimate. */
    char abs_entry[PATH_MAX];
    const char* hashed = realpath(entry_file, abs_entry) ? abs_entry : entry_file;
    unsigned long long entry_hash = 1469598103934665603ull;
    for (const unsigned char* c = (const unsigned char*)hashed; *c; c++) {
      entry_hash = (entry_hash ^ *c) * 1099511628211ull;
    }
    char record_path[PATH_MAX];
    snprintf(record_path, sizeof(record_path), "%s/build-progress-%08llx", record_dir,
             entry_hash & 0xffffffffull);
    progress_begin(ensure_directory_p(record_dir) ? record_path : NULL, progress_last);
  }
  Arena* arena = arena_create(RAE_C_BACKEND_ARENA_CAPACITY);
  if (!arena) {
    diag_fatal("could not allocate arena");
  }
  ModuleGraph graph;
  if (!module_graph_init(&graph, arena, project_root)) {
    arena_destroy(arena);
    progress_end(false);
    return false;
  }
  if (!module_graph_build(&graph, entry_file, NULL, no_implicit)) {
    module_graph_free(&graph);
    arena_destroy(arena);
    progress_end(false);
    return false;
  }
  WatchSources collected_sources;
  watch_sources_init(&collected_sources);
  if (out_sources) {
    if (!module_graph_collect_watch_sources(&graph, &collected_sources)) {
      watch_sources_clear(&collected_sources);
      module_graph_free(&graph);
      arena_destroy(arena);
      progress_end(false);
      return false;
    }
  }
  
  AstModule merged = merge_module_graph(&graph);
  

  bool uses_sdl3 = false;
  for (ModuleNode* node = graph.head; node; node = node->next) {
      // sdl3 and filesystem both define functions in the RAE_HAS_SDL3 runtime
      // block, so either one requires linking libSDL3.
      if (node->module_path && (strcmp(node->module_path, "Sdl3") == 0 || strstr(node->module_path, "/Sdl3.rae") || strstr(node->module_path, "\\Sdl3.rae")
                                || strcmp(node->module_path, "Filesystem") == 0 || strstr(node->module_path, "/Filesystem.rae") || strstr(node->module_path, "\\Filesystem.rae")
                                // Gpu2d owns an SDL3 window (its surface wraps the SDL Metal layer)
                                || strcmp(node->module_path, "Gpu2d") == 0
                                // Gpu3d renders through the same SDL3 window/surface
                                || strcmp(node->module_path, "Gpu3d") == 0)) {
          uses_sdl3 = true;
          break;
      }
  }
  if (out_uses_sdl3) *out_uses_sdl3 = uses_sdl3;

  bool uses_webgpu = false;
  for (ModuleNode* node = graph.head; node; node = node->next) {
      // lib/webgpu.rae (raytracer-specific) OR lib/gpu.rae (generic compute) —
      // both need wgpu-native linked. strstr("gpu.rae") matches both filenames.
      // The generated low-level bindings live under lib/webgpu/ (module paths
      // "webgpu/webgpu", "webgpu/webgpu_types", …), so a plain substring match
      // on "webgpu" catches every generated binding module (#501).
      if (node->module_path && (strstr(node->module_path, "webgpu") || strstr(node->module_path, "Webgpu") ||
                                strcmp(node->module_path, "Gpu") == 0 ||
                                // Gpu2d — presents through wgpu-native (its own render surface)
                                strcmp(node->module_path, "Gpu2d") == 0 ||
                                // Gpu3d — the 3D renderer (MSAA/depth/PBR) on wgpu-native
                                strcmp(node->module_path, "Gpu3d") == 0)) {
          uses_webgpu = true;
          break;
      }
  }
  // General fallback: any module that declares a `cheader` for a wgpu-native
  // header pulls in wgpu-native, even without going through a lib/webgpu module
  // (e.g. a program that binds the C ABI directly). Keeps the detection
  // library-driven rather than hard-coding module names.
  if (!uses_webgpu) {
      for (AstCHeader* hdr = merged.c_headers; hdr; hdr = hdr->next) {
          if (!hdr->path.data) continue;
          char* hpath = str_to_cstr(hdr->path);
          bool is_wgpu = strstr(hpath, "wgpu") != NULL || strstr(hpath, "webgpu") != NULL;
          free(hpath);
          if (is_wgpu) { uses_webgpu = true; break; }
      }
  }
  if (out_uses_webgpu) *out_uses_webgpu = uses_webgpu;

  CompilerContext ctx;
  compiler_init(&ctx, arena);
  
  progress_phase(PROGRESS_SEMA);
  if (!sema_analyze_module(&ctx, &merged)) {
      module_graph_free(&graph);
      arena_destroy(arena);
      progress_end(false);
      return false;
  }

  int errs_before_emit = diag_error_count();
  progress_phase(PROGRESS_EMIT);
  bool ok = c_backend_emit_module(&ctx, &merged, out_file);
  /* The backend reports semantic errors it can only see with full type
   * information (a reference returned to a temporary, for one). Emission
   * still writes a file, so without this the pipeline would hand invalid
   * C to the C compiler and bury the Rae diagnostic under its noise. */
  if (diag_error_count() > errs_before_emit) ok = false;
  if (ok) {
    char out_dir[PATH_MAX];
    strncpy(out_dir, out_file, sizeof(out_dir) - 1);
    out_dir[sizeof(out_dir) - 1] = '\0';
    char* last_slash = strrchr(out_dir, '/');
    if (last_slash) {
      *last_slash = '\0';
      ok = copy_runtime_assets(out_dir);
    } else {
      ok = copy_runtime_assets(".");
    }
  }
  module_graph_free(&graph);
  arena_destroy(arena);
  if (ok && out_sources) {
    watch_sources_move(out_sources, &collected_sources);
  }
  watch_sources_clear(&collected_sources);
  g_build_emit_ms = rae_now_ms() - emit_started_ms;
  if (!ok) progress_end(false);
  return ok;
}

// Invoke GCC to link a Rae-generated `.c` (produced by build_c_backend_output)
// into a runnable binary. Picks up companion `.c` files next to the entry
// `.rae` (excluding the runtime + previous compiled outputs). Legacy Raylib
// is linked only when dependency discovery found an explicit Raylib import.
//
// Shared by `rae run --target compiled` and `rae watch`. Returns true on
// success; on failure, prints to stderr and returns false. The caller owns
// the .c file (we don't unlink it here).
/* Defined with the watch supervisor below; used by the plain `run` path to give
 * each app its own directory (hot-reload channel + window geometry). */
#define RAE_HOT_RELOAD_DIR_ENV "RAE_HOT_RELOAD_DIR"
static bool gcc_link_c_to_binary(const char* entry_rae_file,
                                 const char* c_path,
                                 const char* out_bin,
                                 bool uses_sdl3,
                                 bool uses_webgpu,
                                 int profile) {
  char runtime_dir[PATH_MAX];
  snprintf(runtime_dir, sizeof(runtime_dir), "%s", RAE_RUNTIME_SOURCE_DIR);

  // SDL3 (lib/sdl3.rae): define the runtime block + link libSDL3 only when the
  // program imports it (brew-installed at /opt/homebrew, not toolchain-bundled).
  const char* sdl3_flags = uses_sdl3 ? "-DRAE_HAS_SDL3 -lSDL3" : "";
  // Native WebGPU (lib/webgpu.rae): link wgpu-native + the macOS frameworks it
  // needs, only when imported. Vendored at $WGPU_NATIVE (default ~/.local/
  // wgpu-native); dylib + rpath so the binary finds it at run time.
  char wgpu_flags[PATH_MAX * 2] = {0};
  if (uses_webgpu) {
    const char* wg = getenv("WGPU_NATIVE");
    char wgbuf[PATH_MAX];
    if (!wg || !*wg) {
      const char* home = getenv("HOME"); if (!home) home = ".";
      snprintf(wgbuf, sizeof(wgbuf), "%s/.local/wgpu-native", home);
      wg = wgbuf;
    }
    snprintf(wgpu_flags, sizeof(wgpu_flags),
             "-DRAE_HAS_WEBGPU -I%s/include -L%s/lib -lwgpu_native -Wl,-rpath,%s/lib "
             "-framework Metal -framework QuartzCore -framework CoreFoundation -framework Foundation -framework ImageIO -framework CoreGraphics",
             wg, wg, wg);
  }
  // release: optimize and strip asserts; dev/debug: no opt + symbols so
  // a profiler/debugger reads the generated C and runtime cleanly.
  const char* opt_flags = (profile == BUILD_PROFILE_DEV)
                              ? "-O0 -g"
                              : "-O2 -DNDEBUG";

  char extra_c_files[PATH_MAX * 4] = {0};
  {
    char src_dir[PATH_MAX];
    snprintf(src_dir, sizeof(src_dir), "%s", entry_rae_file);
    char* last_sep = strrchr(src_dir, '/');
    if (last_sep) *last_sep = '\0'; else snprintf(src_dir, sizeof(src_dir), ".");
    DIR* d = opendir(src_dir);
    if (d) {
      struct dirent* ent;
      size_t pos = 0;
      while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen > 2 && strcmp(ent->d_name + nlen - 2, ".c") == 0 &&
            strcmp(ent->d_name, "rae_runtime.c") != 0 && strcmp(ent->d_name, "monocypher.c") != 0 &&
            strcmp(ent->d_name, "lodepng.c") != 0 &&  /* #included by rae_runtime.c — never a standalone TU */
            strncmp(ent->d_name, "rae_compiled_", 13) != 0 && strcmp(ent->d_name, "out.c") != 0) {
          pos += snprintf(extra_c_files + pos, sizeof(extra_c_files) - pos, " %s/%s", src_dir, ent->d_name);
        }
      }
      closedir(d);
    }
  }

  char cmd[PATH_MAX * 4];
  snprintf(cmd, sizeof(cmd), "gcc -std=c11 %s -w %s %s -I%s -I/opt/homebrew/include -L/opt/homebrew/lib -framework Foundation -framework ImageIO -framework CoreGraphics %s %s/rae_runtime.c%s -o %s",
           opt_flags, sdl3_flags, wgpu_flags, runtime_dir,
           c_path, runtime_dir, extra_c_files, out_bin);

  long long cc_started_ms = rae_now_ms();
  progress_phase(PROGRESS_CC);
  int cc_rc = system(cmd);
  progress_end(cc_rc == 0);
  if (cc_rc != 0) {
    fprintf(stderr, "error: failed to compile C output\n");
    return false;
  }
  build_timing_print(entry_rae_file, rae_now_ms() - cc_started_ms);
  return true;
}

/* Link generated C into an Emscripten browser bundle. This is deliberately
 * separate from tools/wasm_build.sh, which targets standalone WASI rather
 * than the browser's SDL3 + WebGPU APIs. */
static bool emcc_link_c_to_web(const char* entry_rae_file,
                               const char* c_path,
                               const char* out_path,
                               bool uses_sdl3,
                               bool uses_webgpu,
                               int profile) {
  if (!ensure_parent_directory(out_path)) return false;

  size_t out_len = strlen(out_path);
  bool emits_html = out_len >= 5 && strcmp(out_path + out_len - 5, ".html") == 0;
  bool emits_module =
      (out_len >= 3 && strcmp(out_path + out_len - 3, ".js") == 0) ||
      (out_len >= 4 && strcmp(out_path + out_len - 4, ".mjs") == 0);
  if (!emits_html && !emits_module) {
    fprintf(stderr, "error: browser WASM output must end in .html, .js, or .mjs\n");
    return false;
  }

  char runtime_c[PATH_MAX];
  char shell_html[PATH_MAX];
  char include_flag[PATH_MAX + 3];
  char assets_path[PATH_MAX];
  char preload_assets[PATH_MAX * 2 + 4];
  /* Shared data the STDLIB reads at runtime, by cwd-relative path. A browser
   * bundle preloading only the example's own assets/ leaves every one of these
   * missing, and each fails quietly in its own way: the settings dialog and
   * camera bar refuse to mount (their .raescene is not there), a Hosek sky
   * cooks from an empty table, and the noise shader comes back blank. That is
   * why 111 and 112 rendered a scene in Chrome but an empty settings panel.
   *
   * Mapped to the SAME path they are read from, so lib code needs no notion of
   * running in a browser. Anything absent is skipped rather than failing the
   * build -- a project that uses none of this should not have to have lib/. */
  static const char* lib_runtime_paths[] = {
    "lib/app3d/scenes",
    "lib/data",
    "lib/noise.wgsl"
  };
  char preload_lib[3][PATH_MAX * 2 + 4];
  int preload_lib_count = 0;
  snprintf(runtime_c, sizeof(runtime_c), "%s/rae_runtime.c", RAE_RUNTIME_SOURCE_DIR);
  snprintf(shell_html, sizeof(shell_html), "%s/web_shell.html", RAE_RUNTIME_SOURCE_DIR);
  snprintf(include_flag, sizeof(include_flag), "-I%s", RAE_RUNTIME_SOURCE_DIR);

  char extra_paths[32][PATH_MAX];
  int extra_count = 0;
  char src_dir[PATH_MAX];
  snprintf(src_dir, sizeof(src_dir), "%s", entry_rae_file);
  char* last_sep = strrchr(src_dir, '/');
  if (last_sep) *last_sep = '\0'; else snprintf(src_dir, sizeof(src_dir), ".");
  snprintf(assets_path, sizeof(assets_path), "%s/assets", src_dir);
  struct stat assets_stat;
  bool has_assets = stat(assets_path, &assets_stat) == 0 && S_ISDIR(assets_stat.st_mode);
  for (size_t i = 0; i < sizeof(lib_runtime_paths) / sizeof(lib_runtime_paths[0]); i++) {
    struct stat lib_stat;
    if (stat(lib_runtime_paths[i], &lib_stat) != 0) continue;
    snprintf(preload_lib[preload_lib_count], sizeof(preload_lib[0]), "%s@/%s",
             lib_runtime_paths[i], lib_runtime_paths[i]);
    preload_lib_count++;
  }
  if (has_assets) {
    /* Preserve the source-relative path expected by Compiled applications.
     * This makes `.raescene`, fonts, images, and other project assets
     * available through the same filesystem API in browser WASM. */
    snprintf(preload_assets, sizeof(preload_assets), "%s@/%s", assets_path, assets_path);
  }
  DIR* d = opendir(src_dir);
  if (d) {
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL && extra_count < 32) {
      size_t nlen = strlen(ent->d_name);
      if (nlen > 2 && strcmp(ent->d_name + nlen - 2, ".c") == 0 &&
          strcmp(ent->d_name, "rae_runtime.c") != 0 &&
          strcmp(ent->d_name, "monocypher.c") != 0 &&
          strcmp(ent->d_name, "lodepng.c") != 0 &&
          strncmp(ent->d_name, "rae_compiled_", 13) != 0 &&
          strcmp(ent->d_name, "out.c") != 0) {
        snprintf(extra_paths[extra_count], PATH_MAX, "%s/%s", src_dir, ent->d_name);
        extra_count++;
      }
    }
    closedir(d);
  }

  const char* args[96];
  int n = 0;
  args[n++] = "emcc";
  /* The runtime uses POSIX helpers such as clock_gettime/getline. Emscripten's
   * strict C11 mode hides those declarations, while its GNU C11 mode exposes
   * the same portable libc surface used by native Compiled builds. */
  args[n++] = "-std=gnu11";
  if (profile == BUILD_PROFILE_DEV) {
    args[n++] = "-O0";
    args[n++] = "-gsource-map";
  } else {
    args[n++] = "-O2";
    args[n++] = "-DNDEBUG";
  }
  args[n++] = "-sALLOW_MEMORY_GROWTH=1";
  args[n++] = "-sASYNCIFY";
  if (emits_html) {
    args[n++] = "--shell-file";
    args[n++] = shell_html;
  } else {
    /* Modular output lets tooling provide its own canvas and lifecycle instead
     * of opening an Emscripten-owned HTML page. */
    args[n++] = "-sMODULARIZE=1";
    args[n++] = "-sEXPORT_ES6=1";
    args[n++] = "-sEXPORT_NAME=createRaeApp";
    /* SDL3 replaces this hook while creating its browser window. Exporting it
     * keeps the modularized Module property writable instead of a legacy
     * read-only compatibility getter. */
    args[n++] = "-sEXPORTED_RUNTIME_METHODS=requestFullscreen";
  }
  if (uses_sdl3) {
    args[n++] = "-DRAE_HAS_SDL3";
    args[n++] = "-sUSE_SDL=3";
  }
  if (uses_webgpu) {
    args[n++] = "-DRAE_HAS_WEBGPU";
    args[n++] = "--use-port=emdawnwebgpu";
  }
  if (has_assets) {
    args[n++] = "--preload-file";
    args[n++] = preload_assets;
  }
  for (int i = 0; i < preload_lib_count; i++) {
    args[n++] = "--preload-file";
    args[n++] = preload_lib[i];
  }
  args[n++] = c_path;
  args[n++] = runtime_c;
  for (int i = 0; i < extra_count; i++) args[n++] = extra_paths[i];
  args[n++] = include_flag;
  args[n++] = "-o";
  args[n++] = out_path;
  args[n] = NULL;

  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "error: could not start emcc: %s\n", strerror(errno));
    return false;
  }
  if (pid == 0) {
    execvp("emcc", (char* const*)args);
    fprintf(stderr, "error: emcc not found; install Emscripten and ensure emcc is on PATH\n");
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "error: Emscripten browser build failed\n");
    return false;
  }
  fprintf(stderr, "built WASM browser bundle: %s\n", out_path);
  return true;
}

// The lib-root: the nearest ancestor of project_root that contains
// lib/core.rae (the repo root for examples, or a standalone `rae init` project
// dir). Used as the exec cwd for run/watch so the `lib/` fallback and
// root-relative asset paths resolve — matching how the devtools runs
// (cwd = repo root, --project = the example dir). Empty out if none.
static void find_lib_root(const char* project_root, char* out, size_t cap) {
  out[0] = '\0';
  if (!project_root || !project_root[0]) return;
  if (!realpath(project_root, out)) snprintf(out, cap, "%s", project_root);
  for (int i = 0; i < 8; i++) {
    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/lib/core/Core.rae", out);  /* #818 */
    if (file_exists(probe)) return;
    char* parent = strrchr(out, '/');
    if (!parent || parent == out) break;
    *parent = '\0';
  }
}

/* #995: run `bin_path` with the program arguments after the `--` of `rae run`
 * as its argv[1..] and wait for it. Returns the raw wait status (0 on a clean
 * exit), like system() did before arguments existed; execv (no shell) is what
 * lets an argument carry spaces or quotes verbatim. */
static int spawn_app_and_wait(const char* bin_path, int app_argc, char** app_argv) {
  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "error: fork failed (%s)\n", strerror(errno));
    return 1;
  }
  if (pid == 0) {
    char** child_argv = calloc((size_t)app_argc + 2, sizeof(char*));
    if (!child_argv) _exit(127);
    child_argv[0] = (char*)bin_path;
    for (int i = 0; i < app_argc; i++) child_argv[i + 1] = app_argv[i];
    child_argv[app_argc + 1] = NULL;
    execv(bin_path, child_argv);
    fprintf(stderr, "error: could not run '%s' (%s)\n", bin_path, strerror(errno));
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return 1;
  }
  return status;
}

static int run_compiled_file(const RunOptions* run_opts, const char* project_root) {
  char temp_c[PATH_MAX];
  char temp_bin[PATH_MAX];

  const char* tmp_dir = getenv("TMPDIR");
  if (!tmp_dir) tmp_dir = "/tmp";

  snprintf(temp_c, sizeof(temp_c), "%s/rae_compiled_%d.c", tmp_dir, getpid());
  snprintf(temp_bin, sizeof(temp_bin), "%s/rae_compiled_%d.bin", tmp_dir, getpid());

  // Resolve the entry to an absolute path, then run the WHOLE pipeline (compile
  // + run) with cwd = project root. Module resolution and asset reads are both
  // cwd-relative, so this makes `rae run` behave identically whether invoked
  // from the repo root, from inside the project/example folder, or by the
  // devtools (which runs from the root). temp_* are absolute, so safe.
  char abs_entry[PATH_MAX];
  const char* file_path = realpath(run_opts->input_path, abs_entry) ? abs_entry : run_opts->input_path;

  // The exec cwd is the lib-root: the nearest ancestor of project_root that
  // contains lib/core.rae (the repo root for examples, or the project dir for a
  // standalone `rae init` project). That makes the `lib/` fallback and
  // root-relative asset paths resolve regardless of where `rae run` was invoked
  // — matching how the devtools runs (cwd = repo root, --project = ENTRY_DIR).
  // Only the zero-config folder mode (`cd <folder>; rae run`) relocates the cwd
  // to the lib-root; explicit `rae run <file>` keeps the invoker's cwd so the
  // test harness and existing scripts are byte-for-byte unchanged.
  char lib_root[PATH_MAX] = {0};
  if (run_opts->zero_config) find_lib_root(project_root, lib_root, sizeof(lib_root));
  char saved_cwd[PATH_MAX];
  bool have_saved = (getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
  bool chdired = false;
  if (lib_root[0] && strcmp(lib_root, ".") != 0) {
    if (chdir(lib_root) == 0) chdired = true;
    else fprintf(stderr, "warning: could not chdir to '%s' (%s)\n",
                 lib_root, strerror(errno));
  }

  bool uses_sdl3 = false;
  bool uses_webgpu = false;
  if (!build_c_backend_output(file_path, project_root, temp_c, run_opts->no_implicit, &uses_sdl3, &uses_webgpu, NULL, PROGRESS_CC)) {
    if (chdired && have_saved) { if (chdir(saved_cwd) != 0) {} }
    return 1;
  }

  if (!gcc_link_c_to_binary(file_path, temp_c, temp_bin, uses_sdl3, uses_webgpu, run_opts->profile)) {
    unlink(temp_c);
    if (chdired && have_saved) { if (chdir(saved_cwd) != 0) {} }
    return 1;
  }

  /* Give this run the same per-app directory `rae watch` gives its children.
   * Nothing writes a reload signal for a plain run, so the hot-reload half is
   * inert — but the WINDOW GEOMETRY lives there too, and without this two
   * examples running side by side share one geometry file and land on top of
   * each other. */
  {
    char cwd_abs[PATH_MAX];
    if (!getcwd(cwd_abs, sizeof(cwd_abs))) snprintf(cwd_abs, sizeof(cwd_abs), ".");
    char channel_id[128];
    watch_channel_id(project_root, file_path, channel_id, sizeof(channel_id));
    char channel_dir[PATH_MAX];
    snprintf(channel_dir, sizeof(channel_dir), "%s/.rae/apps/%s", cwd_abs, channel_id);
    if (ensure_directory_p(channel_dir)) setenv(RAE_HOT_RELOAD_DIR_ENV, channel_dir, 1);
  }
  /* #935: hand the child the toolchain stdlib dir so its runtime shader reads
   * (the lib WGSL assets via rae_ext_rae_gb_read_shader) resolve even when cwd has no
   * local lib/ — matching how #933 lets .rae imports resolve through the
   * toolchain. Do not overwrite a caller-set RAE_STDLIB (the `0`). */
  {
    const char* stdlib = compiler_stdlib_dir();
    if (stdlib && stdlib[0]) setenv("RAE_STDLIB", stdlib, 0);
  }
  /* The crash handler names the program in its one-line report; the temp
   * binary's own name would say nothing, so hand it the entry (relative as
   * the user wrote it) — see runtime_core_memory.c rae_install_crash_handler. */
  setenv("RAE_PROGRAM", run_opts->input_path ? run_opts->input_path : file_path, 1);
  fprintf(stderr, "@@RAE_APP_START@@ entry=%s\n", file_path);
  fflush(stderr);
  long long app_started_ms = rae_now_ms();
  int result = spawn_app_and_wait(temp_bin, run_opts->app_argc, run_opts->app_argv);
  fprintf(stderr, "@@RAE_APP_EXIT@@ entry=%s code=%d run_ms=%lld\n",
          file_path, (result == 0) ? 0 : 1, rae_now_ms() - app_started_ms);
  fflush(stderr);

  if (chdired && have_saved) { if (chdir(saved_cwd) != 0) { /* best effort */ } }
  unlink(temp_c);
  unlink(temp_bin);

  return (result == 0) ? 0 : 1;
}

/* ===================================================================
 * `rae watch` — Phase 3 compiled-mode hot-reload supervisor.
 *
 * High-level shape (see docs/hot-reload-plan.md §4 / §7):
 *
 *   1. Build the entry .rae to `.rae/build/app` and fork+exec it.
 *   2. Snapshot mtimes of the transitive source set via WatchState.
 *   3. Loop: poll mtimes; on a change, rebuild into a side-path
 *      (`.rae/build/app.new`). If that fails, leave the running
 *      child alone — failed builds must not break the dev loop.
 *      If it succeeds, write `.rae/reload.signal` with verb "reload",
 *      wait for the child to exit (it should, after seeing the
 *      signal via lib/hot_reload `pollReloadSignal`), atomically
 *      rename the new binary into place, and fork+exec a new child.
 *   4. SIGINT / SIGTERM: kill the child and exit.
 *
 * Phase 3 deliberately leaves out: content-addressed binaries,
 * previous/current symlink fallback, health-window checks
 * (Phase 4); Live-mode under the same supervisor (Phase 5).
 * The state-file (.rae/state.json) is never touched by the
 * supervisor — apps own that via the stdlib `lib/hot_reload`
 * helpers, and it survives the binary swap because we never
 * delete it.
 * =================================================================== */

extern char g_rae_executable_path[PATH_MAX];

static volatile sig_atomic_t g_watch_stop = 0;
static void watch_sigint_handler(int sig) {
  // Diagnostic line so the devtools / terminal log shows the
  // signal actually reached us. Plain printf is NOT async-signal-
  // safe; use write() to a small fixed buffer so this is safe
  // inside a signal handler. Choosing stderr so it interleaves
  // with the supervisor's main-loop trace.
  const char* msg = "rae watch: SIGINT received, stopping\n";
  if (sig == SIGTERM) msg = "rae watch: SIGTERM received, stopping\n";
  if (sig == SIGHUP)  msg = "rae watch: SIGHUP received, stopping\n";
  ssize_t _w = write(STDERR_FILENO, msg, strlen(msg));
  (void)_w;
  g_watch_stop = 1;
}

// Recursively scan `dir` for `*.rae` files, adding each to `sources`.
// Bounded to a reasonable depth to avoid runaway on symlink loops.
static void watch_collect_rae_sources(const char* dir, WatchSources* sources, int depth) {
  if (depth > 12) return;
  DIR* d = opendir(dir);
  if (!d) return;
  struct dirent* ent;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.') continue;
    if (strcmp(ent->d_name, "build") == 0) continue;
    if (strcmp(ent->d_name, "node_modules") == 0) continue;
    char child[PATH_MAX];
    snprintf(child, sizeof(child), "%s/%s", dir, ent->d_name);
    struct stat st;
    if (stat(child, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      watch_collect_rae_sources(child, sources, depth + 1);
    } else if (S_ISREG(st.st_mode)) {
      size_t nlen = strlen(ent->d_name);
      if (nlen > 4 && strcmp(ent->d_name + nlen - 4, ".rae") == 0) {
        watch_sources_add_file(sources, child);
      }
    }
  }
  closedir(d);
}

// Walk companion `lib/` directories outside the watch root so edits
// to imported stdlib modules (`lib/ui/*.rae`, etc.) trigger rebuilds.
// `try_resolve_lib_module` probes the same locations at compile time;
// without this, the supervisor's glob only saw the project dir and
// silently dropped lib-side change events.
//
// Probes (each walked recursively if it exists):
//   <cwd>/lib                  — repo-root lib when run from there
//   <watch_root>/../lib        — sibling lib next to the example
//   <watch_root>/../../lib     — grand-sibling lib (nested examples)
//
// Dedup happens inside watch_sources_add_file, so it's safe even when
// these paths overlap with the project-root walk.
static void watch_collect_companion_libs(const char* watch_root, WatchSources* sources) {
  const char* candidates[3];
  char cwd_lib[PATH_MAX];
  char sibling_lib[PATH_MAX];
  char grand_lib[PATH_MAX];

  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd))) {
    snprintf(cwd_lib, sizeof(cwd_lib), "%s/lib", cwd);
    candidates[0] = cwd_lib;
  } else {
    candidates[0] = NULL;
  }

  snprintf(sibling_lib, sizeof(sibling_lib), "%s/../lib", watch_root);
  candidates[1] = sibling_lib;
  snprintf(grand_lib, sizeof(grand_lib), "%s/../../lib", watch_root);
  candidates[2] = grand_lib;

  for (int i = 0; i < 3; i++) {
    if (!candidates[i]) continue;
    struct stat st;
    if (stat(candidates[i], &st) != 0) continue;
    if (!S_ISDIR(st.st_mode)) continue;
    watch_collect_rae_sources(candidates[i], sources, 0);
  }
}

// Build the entry .rae to a C source via a fresh `rae build` subprocess.
// Returns true on success; on failure prints a build-error tag (the
// subprocess's stderr is inherited so the actual error is visible above).
static bool watch_subprocess_emit_c(const char* entry, const char* project_root, const char* out_c) {
  if (!g_rae_executable_path[0]) {
    fprintf(stderr, "rae watch: cannot locate rae binary path (argv[0] was empty)\n");
    return false;
  }
  char cmd[PATH_MAX * 4];
  snprintf(cmd, sizeof(cmd),
           "%s build --target compiled --emit-c --out %s --project %s %s",
           g_rae_executable_path, out_c, project_root, entry);
  int rc = system(cmd);
  return rc == 0;
}

// mkdir -p equivalent. The runtime's rae_ext_rae_sys_make_dir only
// creates one level; the supervisor needs nested ".rae/build".
static bool ensure_directory_p(const char* path) {
  if (!path || !*path) return false;
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  size_t len = strlen(tmp);
  if (len == 0) return false;
  if (tmp[len-1] == '/') tmp[len-1] = '\0';
  for (char* p = tmp + 1; *p; ++p) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
  return true;
}

// Atomic file write via tmp + rename.
static bool watch_write_file_atomic(const char* path, const char* body) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE* f = fopen(tmp, "wb");
  if (!f) return false;
  fputs(body, f);
  fclose(f);
  if (rename(tmp, path) != 0) { unlink(tmp); return false; }
  return true;
}

// Atomically write "<verb> <build_id> <epoch-ms>" + newline to
// `<dotrae>/reload.signal`. Children see the mtime advance on the
// next `pollReloadSignal` poll.
static bool watch_write_reload_signal(const char* dotrae_dir,
                                      const char* verb,
                                      const char* build_id) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/reload.signal", dotrae_dir);
  char body[256];
  long long ts_ms = (long long)time(NULL) * 1000;
  snprintf(body, sizeof(body), "%s %s %lld\n", verb, build_id, ts_ms);
  return watch_write_file_atomic(path, body);
}

// Phase 6: surface the last build's outcome to the running app via a
// well-known file. Apps read it with `lib/hot_reload.readBuildStatus`.
//
// Format (newline-separated):
//   line 1: "ok" or "error"
//   line 2: short message (e.g. build id, or a one-line error tag)
static bool watch_write_build_status(const char* dotrae_dir,
                                     bool ok,
                                     const char* message) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/build.status", dotrae_dir);
  char body[1024];
  snprintf(body, sizeof(body), "%s\n%s\n",
           ok ? "ok" : "error",
           message ? message : "");
  return watch_write_file_atomic(path, body);
}

// Poll waitpid until the child exits or timeout_ms elapses.
static bool watch_wait_for_exit(pid_t pid, int timeout_ms, int* out_status) {
  int slept = 0;
  const int step = 50;
  while (slept < timeout_ms) {
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      if (out_status) *out_status = status;
      return true;
    }
    if (r < 0) return false;
    sys_sleep_ms(step);
    slept += step;
  }
  return false;
}

// PER-APP HOT-RELOAD CHANNEL. `.rae/reload.signal` + `.rae/build.status`
// used to be one fixed path per working tree, which was fine while only one
// app could run at a time. With several apps running (devtools can now watch
// more than one), every app polled the SAME signal file, so any supervisor's
// rebuild made every other running app save-state-and-exit. Each supervisor
// now owns `.rae/apps/<id>/` and tells its own children where to look; apps
// launched without a supervisor keep the plain `.rae` default and therefore
// see a file nobody writes.
// Channel id from the project root (or the entry's own directory), reduced to
// path-safe characters so it can name a directory.
static void watch_channel_id(const char* project_root,
                             const char* entry,
                             char* out,
                             size_t out_size) {
  const char* source = (project_root && project_root[0]) ? project_root : entry;
  char trimmed[PATH_MAX];
  snprintf(trimmed, sizeof(trimmed), "%s", source ? source : "app");
  // Drop a trailing slash so basename() of "a/b/" is "b", not "".
  size_t len = strlen(trimmed);
  while (len > 1 && trimmed[len - 1] == '/') { trimmed[--len] = '\0'; }
  const char* base = strrchr(trimmed, '/');
  base = base ? base + 1 : trimmed;
  // An entry path reduces to a file name; strip the extension.
  char name[PATH_MAX];
  snprintf(name, sizeof(name), "%s", base);
  char* dot = strrchr(name, '.');
  if (dot && dot != name) *dot = '\0';

  size_t o = 0;
  for (size_t i = 0; name[i] && o + 1 < out_size; i++) {
    unsigned char c = (unsigned char)name[i];
    bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    out[o++] = safe ? (char)c : '_';
  }
  out[o] = '\0';
  if (o == 0) snprintf(out, out_size, "app");
}

static pid_t watch_spawn_child(const char* bin_path,
                               const char* run_cwd,
                               const char* channel_dir,
                               int app_argc,
                               char** app_argv) {
  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "rae watch: fork failed (%s)\n", strerror(errno));
    return -1;
  }
  if (pid == 0) {
    // The channel dir is absolute precisely because of the chdir below.
    if (channel_dir && channel_dir[0]) setenv(RAE_HOT_RELOAD_DIR_ENV, channel_dir, 1);
    /* #935: same toolchain-stdlib hand-off as run_compiled_file. */
    {
      const char* stdlib = compiler_stdlib_dir();
      if (stdlib && stdlib[0]) setenv("RAE_STDLIB", stdlib, 0);
    }
    // Run with cwd = lib-root so root-relative asset paths resolve (same as
    // `rae run` and the devtools). bin_path is absolute, so this is safe.
    if (run_cwd && run_cwd[0]) { if (chdir(run_cwd) != 0) { /* best effort */ } }
    /* #995: the program arguments after `--` ride along on every restart. */
    char** child_argv = calloc((size_t)app_argc + 2, sizeof(char*));
    if (!child_argv) _exit(127);
    child_argv[0] = (char*)bin_path;
    for (int i = 0; i < app_argc; i++) child_argv[i + 1] = app_argv[i];
    child_argv[app_argc + 1] = NULL;
    execv(bin_path, child_argv);
    fprintf(stderr, "rae watch: execv(%s) failed (%s)\n", bin_path, strerror(errno));
    _exit(127);
  }
  return pid;
}

// Monotonic wall clock in milliseconds. Used for the health window
// (whether a freshly-spawned child has survived long enough to
// promote its build to last-known-good).
static long long watch_now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000 + (long long)tv.tv_usec / 1000;
}

// Wrapper around `kill child + waitpid` with a grace period. SIGTERM
// first, escalate to SIGKILL if the child doesn't go within `grace_ms`.
static void watch_kill_child(pid_t pid, int grace_ms) {
  if (pid <= 0) return;
  kill(pid, SIGTERM);
  if (!watch_wait_for_exit(pid, grace_ms, NULL)) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
  }
}

// Build the entry into a fresh per-build directory `.rae/build/build-N/`.
// On success the binary lives at `<dir>/app`. The C output sits next to it
// so failures are inspectable. Returns true on full success (emit + link).
static bool watch_build_into_dir(const char* entry,
                                 const char* project_root,
                                 const char* build_dir,
                                 char out_bin_path[PATH_MAX],
                                 int profile) {
  if (!ensure_directory_p(build_dir)) return false;
  char c_path[PATH_MAX];
  snprintf(c_path, sizeof(c_path), "%s/app.c", build_dir);
  snprintf(out_bin_path, PATH_MAX, "%s/app", build_dir);

  if (!watch_subprocess_emit_c(entry, project_root, c_path)) return false;
  // SDL3 / wgpu-native are NOT bundled, so we only link them when
  // the program actually imports them — read from the `.deps` sidecar the emit
  // subprocess wrote (build_c_backend_output's import detection). This is what
  // lets `rae watch` build SDL3 / WebGPU examples (e.g. example 53).
  bool uses_sdl3 = false;
  bool uses_webgpu = false;
  {
    char deps_path[PATH_MAX];
    snprintf(deps_path, sizeof(deps_path), "%s.deps", c_path);
    FILE* df = fopen(deps_path, "r");
    if (df) {
      char line[256];
      if (fgets(line, sizeof(line), df)) {
        if (strstr(line, "sdl3")) uses_sdl3 = true;
        if (strstr(line, "webgpu")) uses_webgpu = true;
      }
      fclose(df);
    }
  }
  if (!gcc_link_c_to_binary(entry, c_path, out_bin_path, uses_sdl3, uses_webgpu, profile)) return false;
  return true;
}

static int run_watch_supervisor(const RunOptions* run_opts, const char* project_root) {
  const char* entry = run_opts->input_path;
  // Child apps run with cwd = lib-root so root-relative asset paths resolve
  // (zero-config folder mode only). The supervisor itself stays in the cwd, so
  // .rae/build stays in the folder.
  char lib_root[PATH_MAX] = {0};
  if (run_opts->zero_config) find_lib_root(project_root, lib_root, sizeof(lib_root));

  // Absolute path to cwd: the channel and build dirs hang off it, and the
  // child chdir()s away before it reads them.
  char cwd_abs[PATH_MAX];
  if (!getcwd(cwd_abs, sizeof(cwd_abs))) {
    snprintf(cwd_abs, sizeof(cwd_abs), ".");
  }

  // This supervisor's private hot-reload channel (see watch_channel_id).
  char channel_id[128];
  watch_channel_id(project_root, entry, channel_id, sizeof(channel_id));
  char dotrae[PATH_MAX];
  snprintf(dotrae, sizeof(dotrae), "%s/.rae/apps/%s", cwd_abs, channel_id);
  char channel_build_root[PATH_MAX];
  snprintf(channel_build_root, sizeof(channel_build_root), "%s/build", dotrae);
  if (!ensure_directory_p(channel_build_root)) {
    fprintf(stderr, "rae watch: could not create %s directory\n", channel_build_root);
    return 1;
  }

  // Watch-root: the project root if we found one, otherwise the entry's
  // directory. Source discovery globs recursively under here.
  char watch_root[PATH_MAX];
  if (project_root && project_root[0]) {
    snprintf(watch_root, sizeof(watch_root), "%s", project_root);
  } else {
    snprintf(watch_root, sizeof(watch_root), "%s", entry);
    char* slash = strrchr(watch_root, '/');
    if (slash) *slash = '\0'; else snprintf(watch_root, sizeof(watch_root), ".");
  }

  // ---- Initial build ----
  long long build_seq = 0;
  char current_bin[PATH_MAX] = {0};
  char previous_bin[PATH_MAX] = {0};   // last-known-good (compiled-mode only)
  char build_id[64] = "b0";

  printf("rae watch: target=%s entry=%s\n",
         "compiled", entry);
  fflush(stdout);

  {
    build_seq = 1;
    snprintf(build_id, sizeof(build_id), "b%lld", build_seq);
    char build_dir[PATH_MAX];
    snprintf(build_dir, sizeof(build_dir), "%s/build-%lld", channel_build_root, build_seq);
    printf("rae watch: building %s (%s) ...\n", entry, build_id);
    fflush(stdout);
    if (!watch_build_into_dir(entry, project_root, build_dir, current_bin, run_opts->profile)) {
      fprintf(stderr, "rae watch: initial build failed\n");
      watch_write_build_status(dotrae, false, "initial build failed");
      return 1;
    }
    watch_write_build_status(dotrae, true, build_id);
  }

  // ---- Source-set watcher ----
  WatchSources sources;
  watch_sources_init(&sources);
  watch_collect_rae_sources(watch_root, &sources, 0);
  watch_collect_companion_libs(watch_root, &sources);
  WatchState ws;
  watch_state_init(&ws, entry);
  watch_state_apply_sources(&ws, &sources);

  // ---- Signal handlers ----
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = watch_sigint_handler;
  // Intentionally NOT SA_RESTART: we want the sleep / waitpid in
  // the supervisor loop to return EINTR on signal so g_watch_stop
  // is checked promptly. With SA_RESTART, a SIGTERM arriving
  // during the 150 ms sleep gets swallowed until the sleep
  // naturally completes — which made the devtools Stop button
  // appear to do nothing for up to a frame at a time, and could
  // hang indefinitely if the loop happened to be in a longer
  // wait (waitpid grace window).
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  // SIGHUP is sent when a controlling terminal closes — or when the
  // devtools server process that owns our pseudo-tty / pipe dies.
  // Treat it the same as SIGTERM so a devtools restart cleanly takes
  // us with it instead of orphaning us under launchd.
  sigaction(SIGHUP, &sa, NULL);

  // ---- Spawn first child + arm health window ----
  pid_t child = watch_spawn_child(current_bin, lib_root, dotrae, run_opts->app_argc, run_opts->app_argv);
  if (child < 0) {
    watch_state_free(&ws);
    return 1;
  }
  long long spawn_t = watch_now_ms();
  // Health window: if the child dies non-zero within this many ms of
  // spawn, treat the build as bad and (compiled only) fall back to
  // the previous-good binary.
  const long long HEALTH_MS = 2000;
  long long health_until = spawn_t + HEALTH_MS;
  bool current_promoted = false;  // becomes true once child survives window
  // Crash-resilience: rather than shutting the supervisor down when the app
  // crashes, wait RETRY_MS then rebuild + rerun, forever, until the user stops
  // the watch (Ctrl-C). retry_at != 0 means "an app crashed; rebuild + respawn
  // at this time". A source change during the wait retries immediately.
  const long long RETRY_MS = 60000;
  long long retry_at = 0;

  printf("rae watch: pid=%d running %s\n",
         (int)child, current_bin);
  fflush(stdout);

  while (!g_watch_stop) {
    sys_sleep_ms(150);

    // ---- Reap child if it exited on its own ----
    if (child > 0) {
      int status = 0;
      pid_t r = waitpid(child, &status, WNOHANG);
      if (r == child) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        long long now = watch_now_ms();
        bool in_window = (now < health_until);
        bool bad = (code != 0);

        if (WIFEXITED(status)) {
          printf("rae watch: child exited (code %d)\n", code);
        } else if (WIFSIGNALED(status)) {
          printf("rae watch: child killed (signal %d)\n", WTERMSIG(status));
        }
        fflush(stdout);
        child = -1;

        // Phase 4: bad exit inside the health window → fall back.
        // The supervisor stays alive only in this one case (try the
        // previous-good binary instead of shutting down).
        if (in_window && bad && previous_bin[0]) {
          fprintf(stderr,
                  "rae watch: new build unhealthy; falling back to previous-good\n");
          fflush(stderr);
          watch_write_build_status(dotrae, false, "binary unhealthy; reverted");
          // Restore previous as current.
          strncpy(current_bin, previous_bin, sizeof(current_bin) - 1);
          current_bin[sizeof(current_bin) - 1] = '\0';
          previous_bin[0] = '\0';
          child = watch_spawn_child(current_bin, lib_root, dotrae, run_opts->app_argc, run_opts->app_argv);
          if (child < 0) break;
          spawn_t = watch_now_ms();
          health_until = spawn_t + HEALTH_MS;
          current_promoted = true;   // previous was already good
          printf("rae watch: pid=%d running fallback %s\n", (int)child, current_bin);
          fflush(stdout);
          continue;
        }

        // A crash the fallback above could not handle (post-window, or no
        // previous-good to revert to): do NOT shut the supervisor down.
        // Schedule a rebuild-and-retry after RETRY_MS so a running dev loop
        // survives a crash — edit a source to retry sooner, or press Ctrl-C
        // to stop the watch. (Only reached on unsolicited death; source-change
        // rebuilds reap their own child elsewhere.)
        if (bad) {
          retry_at = watch_now_ms() + RETRY_MS;
          printf("rae watch: app crashed — will rebuild and retry in %llds "
                 "(edit a source to retry sooner, Ctrl-C to stop)\n",
                 (long long)(RETRY_MS / 1000));
          fflush(stdout);
          watch_write_build_status(dotrae, false, "app crashed; will retry");
          continue;
        }

        // Clean exit (code 0). Past the health window it means the user closed
        // the app window — we're done. Inside the window it is a short-lived
        // CLI (e.g. 01_hello prints and returns 0): keep monitoring sources and
        // re-run on edit rather than shutting down.
        if (!in_window) {
          printf("rae watch: child exited cleanly, shutting down supervisor\n");
          fflush(stdout);
          g_watch_stop = 1;
          continue;
        }

        // Clean (or unfallable-back) exit INSIDE the health window:
        // short-running CLI pattern — `01_hello` prints something
        // and returns 0 in well under 2 s. The user asked for
        // `rae watch` to keep monitoring sources and re-run on
        // edit, so don't shut down here; idle until a source change
        // triggers the next build.
      } else if (!current_promoted && watch_now_ms() >= health_until) {
        // Child survived the window → promote.
        current_promoted = true;
      }
    }

    // A scheduled crash-retry fires like a source change: rebuild + respawn.
    bool force_retry = (child < 0 && retry_at != 0 && watch_now_ms() >= retry_at);
    const char* changed = watch_state_poll_change(&ws);
    if (!changed && !force_retry) continue;

    if (changed) {
      printf("rae watch: change in %s, rebuilding ...\n", changed);
    } else {
      printf("rae watch: retrying after crash — rebuilding ...\n");
    }
    fflush(stdout);

    // Refresh the watched-source set every cycle: a new file might
    // have appeared even on a failed build.
    WatchSources new_sources;
    watch_sources_init(&new_sources);
    watch_collect_rae_sources(watch_root, &new_sources, 0);
    watch_collect_companion_libs(watch_root, &new_sources);
    watch_state_apply_sources(&ws, &new_sources);

    char new_bin[PATH_MAX] = {0};
    {
      build_seq += 1;
      snprintf(build_id, sizeof(build_id), "b%lld", build_seq);
      char build_dir[PATH_MAX];
      snprintf(build_dir, sizeof(build_dir),
               "%s/build-%lld", channel_build_root, build_seq);

      bool rebuilt = watch_build_into_dir(entry, project_root, build_dir, new_bin, run_opts->profile);
      watch_state_absorb_formatted(&ws, build_dir);
      if (!rebuilt) {
        char msg[128];
        snprintf(msg, sizeof(msg), "build %s failed", build_id);
        if (child < 0) {
          // No app running (it crashed): the build that would have relaunched
          // it failed too, so wait another RETRY_MS and try again.
          retry_at = watch_now_ms() + RETRY_MS;
          fprintf(stderr, "rae watch: build failed — retrying in %llds "
                  "(edit a source to retry sooner, Ctrl-C to stop)\n",
                  (long long)(RETRY_MS / 1000));
        } else {
          fprintf(stderr, "rae watch: build failed; keeping running app\n");
        }
        fflush(stderr);
        watch_write_build_status(dotrae, false, msg);
        continue;
      }
      watch_write_build_status(dotrae, true, build_id);
    }

    // Tell the running child to save state and exit.
    if (child > 0) {
      watch_write_reload_signal(dotrae, "reload", build_id);
      int wait_status = 0;
      if (!watch_wait_for_exit(child, 2000, &wait_status)) {
        fprintf(stderr, "rae watch: child didn't exit in 2s, forcing\n");
        fflush(stderr);
        watch_kill_child(child, 300);
      }
      child = -1;
    }

    // Promote the previously-running binary to last-known-good (if it
    // survived its own health window). The new bin becomes current.
    {
      if (current_promoted && current_bin[0]) {
        strncpy(previous_bin, current_bin, sizeof(previous_bin) - 1);
        previous_bin[sizeof(previous_bin) - 1] = '\0';
      }
      strncpy(current_bin, new_bin, sizeof(current_bin) - 1);
      current_bin[sizeof(current_bin) - 1] = '\0';
    }

    // Spawn the new child.
    child = watch_spawn_child(current_bin, lib_root, dotrae, run_opts->app_argc, run_opts->app_argv);
    if (child < 0) break;
    spawn_t = watch_now_ms();
    health_until = spawn_t + HEALTH_MS;
    current_promoted = false;
    retry_at = 0;   // running again — clear any pending crash-retry
    printf("rae watch: pid=%d running new build (%s)\n", (int)child, build_id);
    fflush(stdout);
  }

  printf("rae watch: shutting down\n");
  fflush(stdout);
  watch_kill_child(child, 1000);

  watch_state_free(&ws);
  // _exit (not return / not exit) so no atexit handler can keep
  // the supervisor alive past this point. The OS reclaims any
  // remaining state. Without this, a hung atexit handler in the
  // shared rae_runtime would leave the supervisor visible to ps
  // but unkillable via the devtools Stop button.
  fflush(stdout);
  fflush(stderr);
  _exit(0);
}

/* ---- Toolchain requirement check (#931, docs/versioning-and-toolchain.md §2) ---- */

static RaeSemver compiler_semver(void) {
  RaeSemver v = {0, 0, 0};
  rae_semver_parse(RAE_VERSION_BASE, &v);
  return v;
}

/* Scan `dir` (non-recursively) for a single `*.raepack`; on success copy its
 * full path into `out` (size PATH_MAX) and return true. */
static bool dir_has_raepack(const char* dir, char* out) {
  if (!dir) return false;
  DIR* handle = opendir(dir);
  if (!handle) return false;
  bool found = false;
  struct dirent* entry;
  while ((entry = readdir(handle)) != NULL) {
    const char* name = entry->d_name;
    size_t len = strlen(name);
    if (len > 8 && strcmp(name + len - 8, ".raepack") == 0) {
      snprintf(out, PATH_MAX, "%s/%s", dir, name);
      found = true;
      break;
    }
  }
  closedir(handle);
  return found;
}

/* The project's pack lives next to its entry (examples) or in the project
 * root (a `rae init` project, whose entry is under src/). Look in both. */
static bool find_project_pack(const char* entry_path, const char* project_root,
                              char* out) {
  if (entry_path) {
    char entry_dir[PATH_MAX];
    strncpy(entry_dir, entry_path, sizeof(entry_dir) - 1);
    entry_dir[sizeof(entry_dir) - 1] = '\0';
    char* slash = strrchr(entry_dir, '/');
    if (slash) *slash = '\0';
    else strcpy(entry_dir, ".");
    if (dir_has_raepack(entry_dir, out)) return true;
    if (project_root && strcmp(project_root, entry_dir) != 0 &&
        dir_has_raepack(project_root, out)) {
      return true;
    }
    return false;
  }
  return project_root && dir_has_raepack(project_root, out);
}

/* Write (overwrite) the project's rae.lock toolchain block. #933 will add the
 * dependency blocks below it; for now the toolchain block is the whole file. */
static void write_rae_lock(const char* pack_path, const char* version) {
  char lock_path[PATH_MAX];
  strncpy(lock_path, pack_path, sizeof(lock_path) - 1);
  lock_path[sizeof(lock_path) - 1] = '\0';
  char* slash = strrchr(lock_path, '/');
  if (slash) {
    slash[1] = '\0';
    strncat(lock_path, "rae.lock", sizeof(lock_path) - strlen(lock_path) - 1);
  } else {
    strcpy(lock_path, "rae.lock");
  }
  FILE* out = fopen(lock_path, "w");
  if (!out) return;  /* best-effort: a read-only tree must still build */
  fprintf(out, "# generated by rae %s — do not edit\n", version);
  fprintf(out, "toolchain { version: \"%s\" commit: \"%s\" }\n", version,
          RAE_GIT_COMMIT);
  rae_deps_write_lock(out, compiler_stdlib_dir());  /* #933 */
  fclose(out);
}

/* Run the requirement check before the format preflight. Returns true to let
 * the build proceed, false to abort it. `entry_path` may be NULL (zero-config
 * with no discoverable entry) — then only `project_root` is searched. */
static bool toolchain_check(const char* entry_path, const char* project_root,
                            int argc, char** argv) {
  bool strict = false;
  for (int i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--") == 0) break;  /* #995: program args follow */
    if (strcmp(argv[i], "--check-toolchain") == 0) strict = true;
  }
  /* The env override skips the (non-strict) check entirely; --check-toolchain
   * is the CI form that cannot be silenced this way. */
  const char* env = getenv("RAE_TOOLCHAIN_CHECK");
  bool disabled_by_env = env && (strcmp(env, "off") == 0 || strcmp(env, "0") == 0);
  bool skip_version_check = disabled_by_env && !strict;

  char pack_path[PATH_MAX];
  if (!find_project_pack(entry_path, project_root, pack_path)) {
    return true;  /* zero-config project: no pack, no check (packages doc #6) */
  }

  RaePack pack;
  /* Parse non-strict and quiet: a malformed or partial project pack must not
   * turn into a build failure here, nor leak pack diagnostics into the build
   * output — `rae pack` is where pack validity is reported. */
  diag_set_quiet(true);
  bool parsed = raepack_parse_file(pack_path, &pack, false);
  diag_set_quiet(false);
  if (!parsed) return true;
  bool have_requirement = pack.rae_version.len > 0;
  bool have_deps = pack.deps != NULL;
  if (!have_requirement && !have_deps) {
    raepack_free(&pack);
    return true;  /* pack present but declares neither requirement nor deps */
  }

  char version[160];
  build_compiler_version_string(version, sizeof(version));

  /* #933: dependencies resolve regardless of the version check — a build with
   * RAE_TOOLCHAIN_CHECK=off still needs its imports. Pinned by the lock. */
  if (have_deps) {
    char pack_dir[PATH_MAX];
    strncpy(pack_dir, pack_path, sizeof(pack_dir) - 1);
    pack_dir[sizeof(pack_dir) - 1] = '\0';
    char* dslash = strrchr(pack_dir, '/');
    if (dslash) *dslash = '\0'; else strcpy(pack_dir, ".");
    char lock_path[PATH_MAX];
    snprintf(lock_path, sizeof(lock_path), "%s/rae.lock", pack_dir);
    if (!rae_deps_resolve(&pack, pack_dir, lock_path)) {
      raepack_free(&pack);
      return false;
    }
  }
  if (!have_requirement || skip_version_check) {
    write_rae_lock(pack_path, version);
    raepack_free(&pack);
    return true;
  }

  char* req = str_to_cstr(pack.rae_version);
  bool bad_req = false;
  bool satisfied = rae_toolchain_satisfies(req, compiler_semver(),
                                           /*is_dev=*/!RAE_IS_RELEASE, strict,
                                           &bad_req);
  if (bad_req) {
    fprintf(stderr,
            "error: malformed Rae toolchain requirement \"%s\" (in %s).\n"
            "       Use a bare version like \"0.3\" or a range like \">=0.3.0 <0.5.0\".\n",
            req, pack_path);
    free(req);
    raepack_free(&pack);
    return false;
  }

  if (!satisfied) {
    fprintf(stderr,
            "error: this project requires Rae %s (from %s); this compiler is\n"
            "       %s (%s). Run:  rae toolchain use %s\n"
            "       or set RAE_TOOLCHAIN_CHECK=off to build against this compiler anyway.\n",
            req, pack_path, version, RAE_GIT_COMMIT, req);
    free(req);
    raepack_free(&pack);
    return false;
  }

  /* Verified: record the toolchain in the project's lockfile. */
  write_rae_lock(pack_path, version);
  free(req);
  raepack_free(&pack);
  return true;
}

static int run_command(const char* cmd, int argc, char** argv) {
  size_t file_size = 0;
  char* source = NULL;
  Arena* arena = NULL;
  const char* file_path = NULL;
  bool is_format = (strcmp(cmd, "format") == 0);
  bool is_run = (strcmp(cmd, "run") == 0);
  bool is_build = (strcmp(cmd, "build") == 0);
  bool is_pack = (strcmp(cmd, "pack") == 0);
  bool is_watch = (strcmp(cmd, "watch") == 0);

  const char* project_root = NULL;
  char repo_root[PATH_MAX];
  char project_path[PATH_MAX];

  if (is_run || is_build || is_watch) {
      const char* input_path = NULL;
      if (is_run || is_watch) {
          RunOptions run_opts;
          if (parse_run_args(argc, argv, &run_opts)) input_path = run_opts.input_path;
      } else {
          BuildOptions build_opts;
          if (parse_build_args(argc, argv, &build_opts)) input_path = build_opts.entry_path;
      }

      if (input_path) {
          char abs_input[PATH_MAX];
          if (realpath(input_path, abs_input)) {
              strncpy(project_path, abs_input, sizeof(project_path) - 1);
              project_path[sizeof(project_path) - 1] = '\0';
              char* slash = strrchr(project_path, '/');
              if (slash) *slash = '\0';
              else strcpy(project_path, ".");

              strncpy(repo_root, project_path, sizeof(repo_root) - 1);
              repo_root[sizeof(repo_root) - 1] = '\0';
              
              bool found_root = false;
              for (int i = 0; i < 5; ++i) {
                  char test_lib[PATH_MAX];
                  snprintf(test_lib, sizeof(test_lib), "%s/lib/core/Core.rae", repo_root);  /* #818 */
                  if (file_exists(test_lib)) {
                      found_root = true;
                      break;
                  }
                  char* parent = strrchr(repo_root, '/');
                  if (!parent || parent == repo_root) break;
                  *parent = '\0';
              }
              if (found_root) project_root = repo_root;
              else project_root = project_path;
          }
      }
  }

  if (is_format) {
    return rae_format_cli(argc, argv);
  } else if (strcmp(cmd, "lex") == 0 || strcmp(cmd, "parse") == 0) {
    if (argc < 1) {
      fprintf(stderr, "error: %s command requires a file argument\n", cmd);
      print_usage(cmd);
      return 1;
    }
    file_path = argv[0];
                    } else if (is_run) {
                      RunOptions run_opts;
                      if (!parse_run_args(argc, argv, &run_opts)) {
                        print_usage(cmd);
                        return 1;
                      }
                      
                      s_explicit_project_root = (run_opts.project_path != NULL);
                      const char* final_root = run_opts.project_path ? run_opts.project_path : project_root;
                      if (!final_root) final_root = ".";

                      if (!toolchain_check(run_opts.input_path, final_root, argc, argv)) {
                        return 1;
                      }

                      RunOptions adjusted_opts = run_opts;
                      return run_compiled_file(&adjusted_opts, final_root);
              } else if (is_watch) {
                      RunOptions run_opts;
                      if (!parse_run_args(argc, argv, &run_opts)) {
                        print_usage(cmd);
                        return 1;
                      }
                      s_explicit_project_root = (run_opts.project_path != NULL);
                      const char* final_root = run_opts.project_path ? run_opts.project_path : project_root;
                      if (!final_root) final_root = ".";
                      if (!toolchain_check(run_opts.input_path, final_root, argc, argv)) {
                        return 1;
                      }
                      return run_watch_supervisor(&run_opts, final_root);
              } else if (is_pack) {    PackOptions pack_opts;
    if (!parse_pack_args(argc, argv, &pack_opts)) {
      print_usage(cmd);
      return 1;
    }
    return run_raepack_file(&pack_opts);
  } else if (is_build) {
    BuildOptions build_opts;
    if (!parse_build_args(argc, argv, &build_opts)) {
      print_usage(cmd);
      return 1;
    }
    if (!file_exists(build_opts.entry_path)) {
      fprintf(stderr, "error: entry file '%s' not found\n", build_opts.entry_path);
      return 1;
    }
    
    s_explicit_project_root = (build_opts.project_path != NULL);
    const char* final_root = build_opts.project_path ? build_opts.project_path : project_root;
    if (final_root && !directory_exists(final_root)) {
      fprintf(stderr, "error: project path '%s' not found or not a directory\n", final_root);
      return 1;
    }

    if (!toolchain_check(build_opts.entry_path, final_root, argc, argv)) {
      return 1;
    }

    switch (build_opts.target) {
      case BUILD_TARGET_COMPILED: {
        if (!build_opts.emit_c) {
          fprintf(stderr, "error: --emit-c is required for compiled builds\n");
          return 1;
        }
        bool b_sdl3 = false, b_webgpu = false;
        bool okc = build_c_backend_output(build_opts.entry_path,
                                          final_root,
                                          build_opts.out_path,
                                          build_opts.no_implicit,
                                          &b_sdl3,
                                          &b_webgpu,
                                          NULL,
                                          PROGRESS_EMIT);
        progress_end(okc);
        // Record which non-toolchain-bundled libs the program imports next to
        // the emitted C, so `rae watch` (which emits via this subprocess) can
        // link SDL3 / wgpu-native rather than assuming a plain program.
        if (okc) {
          char deps_path[PATH_MAX];
          snprintf(deps_path, sizeof(deps_path), "%s.deps", build_opts.out_path);
          FILE* df = fopen(deps_path, "w");
          if (df) {
            fprintf(df, "%s %s\n",
                    b_sdl3 ? "sdl3" : "-",
                    b_webgpu ? "webgpu" : "-");
            fclose(df);
          }
          /* #919: the files the format preflight rewrote, one per line, so
           * `rae watch` can ignore the mtime events of its own writes. */
          char formatted_path[PATH_MAX];
          snprintf(formatted_path, sizeof(formatted_path), "%s.formatted", build_opts.out_path);
          FILE* ff = fopen(formatted_path, "w");
          if (ff) {
            for (int fi = 0; fi < rae_preflight_rewritten_count(); fi++) {
              fprintf(ff, "%s\n", rae_preflight_rewritten_path(fi));
            }
            fclose(ff);
          }
        }
        return okc ? 0 : 1;
      }
      case BUILD_TARGET_WASM: {
        char temp_c[PATH_MAX];
        snprintf(temp_c, sizeof(temp_c), "/tmp/rae_wasm_%d.c", getpid());
        bool b_sdl3 = false, b_webgpu = false;
        bool okc = build_c_backend_output(build_opts.entry_path,
                                          final_root,
                                          temp_c,
                                          build_opts.no_implicit,
                                          &b_sdl3,
                                          &b_webgpu,
                                          NULL,
                                          PROGRESS_EMIT);
        progress_end(okc);
        bool linked = okc && emcc_link_c_to_web(build_opts.entry_path,
                                                temp_c,
                                                build_opts.out_path,
                                                b_sdl3,
                                                b_webgpu,
                                                build_opts.profile);
        unlink(temp_c);
        return linked ? 0 : 1;
      }
      default:
        fprintf(stderr, "error: unsupported build target\n");
        return 1;
    }
  } else {
    fprintf(stderr, "error: unknown command '%s'\n", cmd);
    print_usage(cmd);
    return 1;
  }

  source = read_file(file_path, &file_size);
  if (!source) {
    fprintf(stderr, "error: could not read file '%s'\n", file_path);
    return 1;
  }
  arena = arena_create(64 * 1024 * 1024);
  if (!arena) {
    free(source);
    diag_fatal("could not allocate arena");
  }
  TokenList tokens = lexer_tokenize(arena, file_path, source, file_size, !is_format);
  if (tokens.had_error && !is_format) {
      exit(1);
  }

  if (strcmp(cmd, "lex") == 0) {
    dump_tokens(&tokens);
  } else if (strcmp(cmd, "parse") == 0) {
    AstModule* module = parse_module(arena, file_path, tokens);
    ast_dump_module(module, stdout);
  }

  arena_destroy(arena);
  free(source);
  return diag_error_count() > 0 ? 1 : 0;
}

// Absolute path to the rae executable, captured at process start.
// The watch supervisor shells out to a fresh `rae build` subprocess for
// each rebuild so the compiler runs in a clean address space (parts of
// c_backend / mangler keep arena-backed pointers in module-level state
// that don't survive arena_destroy between in-process builds; the
// subprocess approach side-steps that bug while we work on the fix).
char g_rae_executable_path[PATH_MAX] = {0};

// ----- rae init: scaffold a Rae project in $PWD --------------------
//
// Idempotent. Every step checks whether the target already exists
// and prints "already exists, leaving alone" rather than overwriting.
// The Roboto download uses curl; if curl is missing or the host is
// offline, we report the failure and continue with the rest of the
// scaffolding so the user can drop the font in manually.

static const char* RAE_INIT_TEMPLATE_MAIN_RAE =
  "# Hello-world generated by `rae init`. Edit me, then `make run`.\n"
  "\n"
  "func main() {\n"
  "  log(\"Hello, Rae!\")\n"
  "}\n";

// Tabs in the recipe lines below are real \t — required by make. The generated
// starter is dependency-light; apps add SDL3/WebGPU flags when they import
// those platform modules.
/* #919: every new project carries the formatting section of AGENTS.md so an
 * agent working in it knows the compiler owns layout. */
static const char* RAE_INIT_TEMPLATE_AGENTS_MD =
  "# AGENTS.md — working in this Rae project\n"
  "\n"
  "## Source formatting: `rae format` owns layout\n"
  "\n"
  "Rae has ONE canonical layout and the compiler is its authority — there is no\n"
  "style configuration.\n"
  "\n"
  "- 2-space indent, 100 columns, LF, one final newline, no trailing whitespace,\n"
  "  braces on the declaration line; files are capped at 1,000 lines.\n"
  "- Function parameters and call arguments: 1-3 items may share the line if the\n"
  "  whole header/call fits in 100 columns; from FOUR items, or whenever it does\n"
  "  not fit, every item goes on its own line (two spaces deeper, no commas, `)`\n"
  "  back at the declaration's indentation). Object and collection literals and\n"
  "  enum members: the same from FIVE items. A `type` declaration is always one\n"
  "  field per line. Never hand-align, never keep personal wrapping.\n"
  "- The compiler formats FIRST: `rae run` / `rae build` / `rae watch` rewrite\n"
  "  the project's changed `.rae` files canonically (atomic, in place) before\n"
  "  compiling. `--check-format` or `RAE_FORMAT=check` refuse to write and fail\n"
  "  with the file list instead (use that in CI). A file whose canonical form\n"
  "  would exceed 1,000 lines is an error to split, never a rewrite.\n"
  "- `rae format <files|dirs>` formats in place; `--check` lists unformatted\n"
  "  files, `--stdout` prints, `--stdin --stdout` filters, `--rules --json`\n"
  "  prints the rules. `# raefmt: off` / `# raefmt: on` fence a verbatim region\n"
  "  (rare: foreign snippets, aligned tables).\n";

static const char* RAE_INIT_TEMPLATE_MAKEFILE =
  "# Generated by `rae init`. Set RAE_REPO if the Rae monorepo lives\n"
  "# somewhere other than ../rae, e.g. `make RAE_REPO=/abs/path`.\n"
  "\n"
  "SHELL := /bin/zsh\n"
  ".DEFAULT_GOAL := run\n"
  "\n"
  "RAE_REPO ?= $(realpath $(CURDIR)/../rae)\n"
  "RAE_BIN  := $(RAE_REPO)/compiler/bin/rae\n"
  "\n"
  "ENTRY    := $(CURDIR)/src/Main.rae\n"
  "PROJECT  := $(CURDIR)\n"
  "BUILD    := $(CURDIR)/build\n"
  "OUT_C    := $(BUILD)/main.c\n"
  "RUNTIME_C:= $(BUILD)/rae_runtime.c\n"
  "BIN      := $(BUILD)/app\n"
  "\n"
  "CFLAGS   := -O2 -I/opt/homebrew/include\n"
  "LDFLAGS  := -L/opt/homebrew/lib -framework Foundation \\\n"
  "            -framework ImageIO -framework CoreGraphics\n"
  "\n"
  ".PHONY: help build run watch clean lib-link\n"
  "\n"
  "help:\n"
  "\t@echo \"Targets: make / make run / make build / make watch / make clean\"\n"
  "\n"
  "# Symlink the rae stdlib into <project>/lib/ so the compiler's\n"
  "# resolver finds it without a vendored copy. Idempotent.\n"
  "lib-link:\n"
  "\t@ln -sfn $(RAE_REPO)/lib lib\n"
  "\n"
  "$(BUILD):\n"
  "\t@mkdir -p $(BUILD)\n"
  "\n"
  "build: lib-link $(BUILD)\n"
  "\t$(RAE_BIN) build --target compiled --emit-c \\\n"
  "\t    --project $(PROJECT) --entry $(ENTRY) --out $(OUT_C)\n"
  "\tgcc $(CFLAGS) -o $(BIN) $(OUT_C) $(RUNTIME_C) -I$(BUILD) $(LDFLAGS)\n"
  "\n"
  "run: build\n"
  "\t"  "RAE_STDLIB=$(RAE_REPO)/lib "  "$(BIN)\n"
  "\n"
  "watch: lib-link\n"
  "\t$(RAE_BIN) watch --target compiled --project $(PROJECT) $(ENTRY)\n"
  "\n"
  "clean:\n"
  "\trm -rf $(BUILD)\n"
  "\t@rm -f lib\n";

// The googlefonts/roboto repo on GitHub keeps the hinted static
// Roboto-Regular cut at a stable path. google/fonts (the
// distribution mirror) moved Roboto to a variable-font layout in
// 2022 and the old static/ folder no longer resolves; this
// upstream repo still ships the classic TTF.
static const char* RAE_INIT_ROBOTO_URL =
  "https://github.com/googlefonts/roboto/raw/main/"
  "src/hinted/Roboto-Regular.ttf";

static int rae_init_ensure_dir(const char* name) {
  if (mkdir(name, 0755) == 0) {
    printf("  created %s/\n", name);
    return 0;
  }
  if (errno == EEXIST) {
    printf("  %s/ already exists, leaving alone\n", name);
    return 0;
  }
  fprintf(stderr, "  mkdir %s: %s\n", name, strerror(errno));
  return 1;
}

static int rae_init_write_if_missing(const char* path, const char* contents) {
  if (file_exists(path)) {
    printf("  %s already exists, leaving alone\n", path);
    return 0;
  }
  FILE* f = fopen(path, "w");
  if (!f) {
    fprintf(stderr, "  open %s for write: %s\n", path, strerror(errno));
    return 1;
  }
  size_t len = strlen(contents);
  size_t wrote = fwrite(contents, 1, len, f);
  fclose(f);
  if (wrote != len) {
    fprintf(stderr, "  partial write to %s\n", path);
    return 1;
  }
  printf("  created %s\n", path);
  return 0;
}

static int rae_init_download_roboto(void) {
  const char* path = "assets/Roboto-Regular.ttf";
  if (file_exists(path)) {
    printf("  %s already exists, leaving alone\n", path);
    return 0;
  }
  printf("  downloading %s from %s\n", path, RAE_INIT_ROBOTO_URL);
  // -f fail on HTTP error; -s silent progress; -S still show errors;
  // -L follow redirects (raw.githubusercontent currently doesn't redirect
  // but the flag is cheap insurance). --create-dirs covers the assets/
  // already-created case in one go.
  char cmd[1024];
  int n = snprintf(cmd, sizeof(cmd),
                   "curl -fsSL --create-dirs -o '%s' '%s'",
                   path, RAE_INIT_ROBOTO_URL);
  if (n < 0 || (size_t)n >= sizeof(cmd)) {
    fprintf(stderr, "  command line too long\n");
    return 1;
  }
  int rc = system(cmd);
  if (rc != 0) {
    fprintf(stderr,
            "  curl failed (exit %d). Skipping font; drop a TTF at\n"
            "  %s manually if you need one.\n",
            rc, path);
    // Non-fatal: the other scaffolding still proceeds. Surface the
    // failure to the caller's exit code so CI can spot it though.
    return 1;
  }
  printf("  created %s\n", path);
  return 0;
}

static int cmd_init(int argc, char** argv) {
  (void)argc; (void)argv;
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) {
    fprintf(stderr, "rae init: getcwd failed: %s\n", strerror(errno));
    return 1;
  }
  printf("rae init: scaffolding under %s\n", cwd);

  int rc = 0;
  rc |= rae_init_ensure_dir("src");
  rc |= rae_init_ensure_dir("assets");
  rc |= rae_init_write_if_missing("src/Main.rae", RAE_INIT_TEMPLATE_MAIN_RAE);
  // Font fetch is best-effort: keep going past a download failure so
  // the Makefile + src layout still get written.
  int font_rc = rae_init_download_roboto();
  rc |= rae_init_write_if_missing("Makefile", RAE_INIT_TEMPLATE_MAKEFILE);
  rc |= rae_init_write_if_missing("AGENTS.md", RAE_INIT_TEMPLATE_AGENTS_MD);

  // #931: declare the toolchain requirement so a second machine knows which
  // Rae this project builds against. The caret is on this compiler's MINOR
  // (the breaking axis pre-1.0), e.g. compiler 0.1.x -> `rae: { version: "0.1" }`.
  RaeSemver self = compiler_semver();
  char pack_contents[1024];
  snprintf(pack_contents, sizeof(pack_contents),
           "pack App {\n"
           "  format: \"raepack\"\n"
           "  version: 1\n"
           "  defaultTarget: compiled\n"
           "  rae: {\n"
           "    version: \"%d.%d\"\n"
           "  }\n"
           "  targets: {\n"
           "    target compiled: {\n"
           "      label: \"Compiled\"\n"
           "      entry: \"src/Main.rae\"\n"
           "      sources: {\n"
           "        source: { path: \"src\", emit: compiled }\n"
           "      }\n"
           "    }\n"
           "  }\n"
           "}\n",
           self.major, self.minor);
  rc |= rae_init_write_if_missing("App.raepack", pack_contents);

  if (rc != 0 || font_rc != 0) {
    fprintf(stderr,
            "rae init: finished with warnings — review the messages above\n");
    return 1;
  }
  printf("rae init: done\n");
  return 0;
}

/* ---- `rae toolchain` (#932, docs/versioning-and-toolchain.md §3) ---- */

/* The git checkout this compiler was built from: $RAE_ROOT when set, else the
 * parent of the stdlib dir (the binary lives at <root>/compiler/bin/rae and
 * the stdlib at <root>/lib). Returns false when it cannot be determined. */
static bool rae_checkout_root(char* out, size_t out_len) {
  const char* env = getenv("RAE_ROOT");
  if (env && env[0]) {
    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/compiler", env);
    if (directory_exists(probe)) {
      snprintf(out, out_len, "%s", env);
      return true;
    }
  }
  const char* lib = compiler_stdlib_dir();
  if (!lib) return false;
  char root[PATH_MAX];
  snprintf(root, sizeof(root), "%s", lib);
  char* slash = strrchr(root, '/');
  if (!slash || slash == root) return false;
  *slash = '\0';  /* strip trailing "/lib" -> checkout root */
  snprintf(out, out_len, "%s", root);
  return true;
}

/* Run `git -C <root> <args>` capturing stdout into `out` (newline-trimmed).
 * Returns the command's success (exit 0). */
static bool git_capture(const char* root, const char* args, char* out, size_t out_len) {
  char cmd[PATH_MAX + 256];
  snprintf(cmd, sizeof(cmd), "git -C \"%s\" %s 2>/dev/null", root, args);
  FILE* pipe = popen(cmd, "r");
  if (!pipe) return false;
  size_t n = fread(out, 1, out_len - 1, pipe);
  out[n] = '\0';
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
  int rc = pclose(pipe);
  return rc == 0;
}

/* Tracked-file changes only (matches `git describe --dirty` / #930's dirty
 * rule): untracked files do not count, so a stray build/ output or an
 * uncommitted metrics file does not block a toolchain switch. */
static bool checkout_is_dirty(const char* root) {
  char cmd[2 * PATH_MAX + 128];
  snprintf(cmd, sizeof(cmd),
           "git -C \"%s\" diff --quiet && git -C \"%s\" diff --cached --quiet",
           root, root);
  return system(cmd) != 0;
}

/* Print the semver tags (v*.*.* only) newest-first, marking the current one. */
static void toolchain_list_tags(const char* root) {
  char tags[8192];
  if (!git_capture(root,
                   "tag --list 'v[0-9]*.[0-9]*.[0-9]*' --sort=-v:refname",
                   tags, sizeof(tags)) ||
      tags[0] == '\0') {
    printf("no release tags in %s\n", root);
    return;
  }
  const char* current = RAE_IS_RELEASE ? RAE_GIT_TAG : "";
  char* saveptr = NULL;
  for (char* line = strtok_r(tags, "\n", &saveptr); line;
       line = strtok_r(NULL, "\n", &saveptr)) {
    bool is_current = current[0] && strcmp(line, current) == 0;
    printf("%s %s%s\n", is_current ? "*" : " ", line,
           is_current ? "  (current)" : "");
  }
}

/* Resolve requirement `req` (leading 'v' tolerated) to the newest v*.*.* tag
 * that satisfies it. Copies the tag into `out`; returns false if none match. */
static bool toolchain_resolve_tag(const char* root, const char* req, char* out,
                                  size_t out_len) {
  if (req[0] == 'v' || req[0] == 'V') req += 1;
  char tags[8192];
  if (!git_capture(root, "tag --list 'v[0-9]*.[0-9]*.[0-9]*'", tags, sizeof(tags)) ||
      tags[0] == '\0') {
    return false;
  }
  bool found = false;
  RaeSemver best = {0, 0, 0};
  char* saveptr = NULL;
  for (char* line = strtok_r(tags, "\n", &saveptr); line;
       line = strtok_r(NULL, "\n", &saveptr)) {
    RaeSemver candidate;
    if (!rae_semver_parse(line[0] == 'v' ? line + 1 : line, &candidate)) continue;
    bool bad_req = false;
    if (!rae_toolchain_satisfies(req, candidate, /*is_dev=*/false,
                                 /*strict=*/false, &bad_req)) {
      if (bad_req) return false;  /* the requirement itself is malformed */
      continue;
    }
    bool newer = !found ||
                 candidate.major > best.major ||
                 (candidate.major == best.major && candidate.minor > best.minor) ||
                 (candidate.major == best.major && candidate.minor == best.minor &&
                  candidate.patch > best.patch);
    if (newer) {
      best = candidate;
      snprintf(out, out_len, "%s", line);
      found = true;
    }
  }
  return found;
}

static int toolchain_status(const char* root) {
  char version[160];
  build_compiler_version_string(version, sizeof(version));
  printf("rae %s (%s %s)\n", version, RAE_GIT_COMMIT, RAE_GIT_DATE);
  printf("checkout: %s\n", root ? root : "(unknown)");

  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) return 0;
  char pack_path[PATH_MAX];
  if (!find_project_pack(NULL, cwd, pack_path)) {
    printf("project: no .raepack here (zero-config; no toolchain requirement)\n");
    return 0;
  }
  RaePack pack;
  diag_set_quiet(true);
  bool parsed = raepack_parse_file(pack_path, &pack, false);
  diag_set_quiet(false);
  if (!parsed || pack.rae_version.len == 0) {
    if (parsed) raepack_free(&pack);
    printf("project: %s (no rae requirement)\n", pack_path);
    return 0;
  }
  char* req = str_to_cstr(pack.rae_version);
  bool bad_req = false;
  bool ok = rae_toolchain_satisfies(req, compiler_semver(), /*is_dev=*/!RAE_IS_RELEASE,
                                    /*strict=*/false, &bad_req);
  printf("project: %s requires Rae %s\n", pack_path, req);
  if (bad_req) {
    printf("verdict: malformed requirement\n");
    free(req);
    raepack_free(&pack);
    return 1;
  }
  printf("verdict: %s\n", ok ? "satisfied" : "UNSATISFIED — run: rae toolchain use");
  free(req);
  raepack_free(&pack);
  return ok ? 0 : 1;
}

static int toolchain_use(const char* root, const char* req) {
  bool to_main = strcmp(req, "main") == 0;
  char target[128];
  if (to_main) {
    snprintf(target, sizeof(target), "main");
  } else {
    /* Pull tags first (best-effort: offline still resolves against local
     * tags). fetch touches only .git, so it is safe on a dirty tree. */
    char scratch[256];
    git_capture(root, "fetch --tags --quiet", scratch, sizeof(scratch));
    if (!toolchain_resolve_tag(root, req, target, sizeof(target))) {
      fprintf(stderr,
              "error: no release tag in %s satisfies \"%s\".\n"
              "       Available tags:\n", root, req);
      toolchain_list_tags(root);
      return 1;
    }
  }

  if (checkout_is_dirty(root)) {
    fprintf(stderr,
            "error: the Rae checkout %s has uncommitted changes; refusing to\n"
            "       switch toolchain (your in-flight work would be left on a\n"
            "       detached HEAD). Commit or stash it, then retry.\n",
            root);
    return 1;
  }

  printf("rae toolchain: checking out %s in %s\n", target, root);
  char cmd[PATH_MAX + 128];
  snprintf(cmd, sizeof(cmd), "git -C \"%s\" checkout %s", root, target);
  if (system(cmd) != 0) {
    fprintf(stderr, "error: git checkout %s failed\n", target);
    return 1;
  }
  printf("rae toolchain: rebuilding compiler (make -C %s/compiler build)\n", root);
  snprintf(cmd, sizeof(cmd), "make -C \"%s/compiler\" build", root);
  if (system(cmd) != 0) {
    fprintf(stderr, "error: rebuilding the compiler failed\n");
    return 1;
  }
  printf("rae toolchain: now on %s. Run 'rae --version' to confirm.\n", target);
  return 0;
}

static void toolchain_usage(void) {
  fprintf(stderr, "Usage: rae toolchain <status|use|list>\n");
  fprintf(stderr, "  status        Print this compiler's version + checkout and,\n");
  fprintf(stderr, "                inside a project, the pack's requirement + verdict\n");
  fprintf(stderr, "  use <req>     Check out the newest tag matching <req> (a version\n");
  fprintf(stderr, "                like 0.3, or 'main' for the development head) and\n");
  fprintf(stderr, "                rebuild the compiler; refuses on a dirty checkout\n");
  fprintf(stderr, "  list          List release tags, marking the current one\n");
}

static int cmd_toolchain(int argc, char** argv) {
  char root[PATH_MAX];
  bool have_root = rae_checkout_root(root, sizeof(root));

  const char* sub = argc >= 1 ? argv[0] : "status";
  if (strcmp(sub, "status") == 0) {
    return toolchain_status(have_root ? root : NULL);
  }
  if (strcmp(sub, "list") == 0) {
    if (!have_root) {
      fprintf(stderr, "error: cannot locate the Rae checkout (set $RAE_ROOT)\n");
      return 1;
    }
    toolchain_list_tags(root);
    return 0;
  }
  if (strcmp(sub, "use") == 0) {
    if (argc < 2) {
      fprintf(stderr, "error: 'rae toolchain use' needs a version or 'main'\n");
      return 1;
    }
    if (!have_root) {
      fprintf(stderr, "error: cannot locate the Rae checkout (set $RAE_ROOT)\n");
      return 1;
    }
    return toolchain_use(root, argv[1]);
  }
  if (strcmp(sub, "-h") == 0 || strcmp(sub, "--help") == 0) {
    toolchain_usage();
    return 0;
  }
  fprintf(stderr, "error: unknown toolchain subcommand '%s'\n", sub);
  toolchain_usage();
  return 1;
}


/* ---- `rae add` / `fetch` / `update` / `tree` (#934, docs/versioning-and-toolchain.md §4) ---- */

/* Locate the project pack (in cwd) into `pack_path`; false with a message when
 * there is none. Also fills `pack_dir` with the directory the pack lives in. */
static bool packages_find_pack(char* pack_path, char* pack_dir) {
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) {
    fprintf(stderr, "error: getcwd failed: %s\n", strerror(errno));
    return false;
  }
  if (!find_project_pack(NULL, cwd, pack_path)) {
    fprintf(stderr, "error: no .raepack in %s — dependencies live in a pack\n", cwd);
    return false;
  }
  strncpy(pack_dir, pack_path, PATH_MAX - 1);
  pack_dir[PATH_MAX - 1] = '\0';
  char* slash = strrchr(pack_dir, '/');
  if (slash) *slash = '\0'; else strcpy(pack_dir, ".");
  return true;
}

/* Index of the `}` matching the `{` at s[open]. Returns -1 if unbalanced. */
static long matching_brace(const char* s, long open) {
  int depth = 0;
  for (long i = open; s[i]; ++i) {
    if (s[i] == '{') depth += 1;
    else if (s[i] == '}') { depth -= 1; if (depth == 0) return i; }
  }
  return -1;
}

/* Edit the pack text to add one dependency; returns a malloc'd new buffer, or
 * NULL on a structural problem (message printed). */
static char* packages_insert_dep(const char* text, const char* entry) {
  const char* deps = strstr(text, "dependencies");
  size_t entry_len = strlen(entry);
  if (deps) {
    const char* brace = strchr(deps, '{');
    if (!brace) { fprintf(stderr, "error: malformed dependencies block\n"); return NULL; }
    long open = brace - text;
    long close = matching_brace(text, open);
    if (close < 0) { fprintf(stderr, "error: unbalanced dependencies block\n"); return NULL; }
    /* Splice the entry in at the START of the closing brace's line, so that
     * brace keeps its own indentation and the new dep lines up with siblings. */
    long at = close;
    while (at > open && (text[at - 1] == ' ' || text[at - 1] == '\t')) at -= 1;
    char* out = malloc(strlen(text) + entry_len + 2);
    if (!out) return NULL;
    memcpy(out, text, at);
    out[at] = '\0';
    strcat(out, entry);
    strcat(out, text + at);
    return out;
  }
  /* No dependencies block yet: add one just before `targets` (required field). */
  const char* targets = strstr(text, "targets");
  const char* insert_at = targets ? targets : NULL;
  /* Back up to the start of the targets line so indentation is preserved. */
  if (insert_at) {
    while (insert_at > text && insert_at[-1] != '\n') insert_at -= 1;
  } else {
    fprintf(stderr, "error: pack has no targets block to anchor dependencies before\n");
    return NULL;
  }
  long at = insert_at - text;
  const char* head = "  dependencies: {\n";
  const char* tail = "  }\n";
  size_t block_len = strlen(head) + entry_len + strlen(tail);
  char* out = malloc(strlen(text) + block_len + 2);
  if (!out) return NULL;
  memcpy(out, text, at);
  out[at] = '\0';
  strcat(out, head);
  strcat(out, entry);
  strcat(out, tail);
  strcat(out, text + at);
  return out;
}

static int cmd_add(int argc, char** argv) {
  const char* name = NULL;
  const char* path = NULL;
  const char* git = NULL;
  const char* rev = NULL;
  for (int i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) { path = argv[++i]; }
    else if (strcmp(argv[i], "--git") == 0 && i + 1 < argc) { git = argv[++i]; }
    else if (strcmp(argv[i], "--rev") == 0 && i + 1 < argc) { rev = argv[++i]; }
    else if (argv[i][0] == '-') { fprintf(stderr, "error: unknown add option '%s'\n", argv[i]); return 1; }
    else if (!name) { name = argv[i]; }
    else { fprintf(stderr, "error: unexpected argument '%s'\n", argv[i]); return 1; }
  }
  if (!name) { fprintf(stderr, "error: 'rae add' needs a dependency name\n"); return 1; }
  if ((path != NULL) == (git != NULL)) {
    fprintf(stderr, "error: give exactly one of --path or --git\n");
    return 1;
  }
  if (rev && !git) { fprintf(stderr, "error: --rev is only valid with --git\n"); return 1; }

  char pack_path[PATH_MAX], pack_dir[PATH_MAX];
  if (!packages_find_pack(pack_path, pack_dir)) return 1;

  /* Refuse a duplicate name up front (clearer than a post-edit parse error). */
  RaePack existing;
  diag_set_quiet(true);
  bool parsed_existing = raepack_parse_file(pack_path, &existing, false);
  diag_set_quiet(false);
  if (parsed_existing) {
    for (const RaePackDep* dep = existing.deps; dep; dep = dep->next) {
      if (str_eq_cstr(dep->name, name)) {
        fprintf(stderr, "error: dependency '%s' is already declared\n", name);
        raepack_free(&existing);
        return 1;
      }
    }
    raepack_free(&existing);
  }

  char entry[2 * PATH_MAX];
  if (path) {
    snprintf(entry, sizeof(entry),
             "    dep %s: {\n      path: \"%s\"\n    }\n", name, path);
  } else if (rev) {
    snprintf(entry, sizeof(entry),
             "    dep %s: {\n      git: \"%s\"\n      rev: \"%s\"\n    }\n", name, git, rev);
  } else {
    snprintf(entry, sizeof(entry),
             "    dep %s: {\n      git: \"%s\"\n    }\n", name, git);
  }

  size_t size = 0;
  char* text = read_file(pack_path, &size);
  if (!text) { fprintf(stderr, "error: could not read %s\n", pack_path); return 1; }
  char* edited = packages_insert_dep(text, entry);
  free(text);
  if (!edited) return 1;

  /* Guard the mechanical splice with a brace-balance check (a full parse would
   * couple this metadata edit to entry files existing, which need not be true
   * when scaffolding). Write atomically via temp + rename. */
  int depth = 0;
  bool balanced = true;
  for (const char* p = edited; *p; ++p) {
    if (*p == '{') depth += 1;
    else if (*p == '}') { depth -= 1; if (depth < 0) { balanced = false; break; } }
  }
  if (!balanced || depth != 0) {
    fprintf(stderr, "error: the edit unbalanced the pack braces; left %s unchanged\n", pack_path);
    free(edited);
    return 1;
  }
  char tmp_path[PATH_MAX];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", pack_path);
  FILE* tmp = fopen(tmp_path, "w");
  if (!tmp) { fprintf(stderr, "error: could not write %s\n", tmp_path); free(edited); return 1; }
  fputs(edited, tmp);
  fclose(tmp);
  free(edited);
  if (rename(tmp_path, pack_path) != 0) {
    remove(tmp_path);
    fprintf(stderr, "error: could not update %s\n", pack_path);
    return 1;
  }
  printf("added dependency '%s' to %s\n", name, pack_path);
  printf("run 'rae fetch' (or a build) to resolve it into rae.lock\n");
  return 0;
}

/* Shared resolve step for fetch/update/tree: parse the pack, resolve deps into
 * .rae/deps. `update_name` (NULL) / update_all drive re-resolution. Returns the
 * parsed pack in *out (caller frees) or false on failure. */
static bool packages_resolve(const char* pack_path, const char* pack_dir, RaePack* out) {
  if (!raepack_parse_file(pack_path, out, false)) {
    fprintf(stderr, "error: could not parse %s\n", pack_path);
    return false;
  }
  char lock_path[PATH_MAX];
  snprintf(lock_path, sizeof(lock_path), "%s/rae.lock", pack_dir);
  if (!rae_deps_resolve(out, pack_dir, lock_path)) {
    raepack_free(out);
    return false;
  }
  return true;
}

static int cmd_fetch(void) {
  char pack_path[PATH_MAX], pack_dir[PATH_MAX];
  if (!packages_find_pack(pack_path, pack_dir)) return 1;
  char lock_path[PATH_MAX];
  snprintf(lock_path, sizeof(lock_path), "%s/rae.lock", pack_dir);
  bool had_lock = file_exists(lock_path);
  RaePack pack;
  if (!packages_resolve(pack_path, pack_dir, &pack)) return 1;
  /* fetch honors an existing lock and never rewrites it (deterministic,
   * offline-safe); with no lock yet it captures the resolved state once. */
  if (!had_lock) {
    char version[160];
    build_compiler_version_string(version, sizeof(version));
    write_rae_lock(pack_path, version);
  }
  printf("fetched %d dependenc%s into %s/.rae/deps\n",
         rae_deps_count(), rae_deps_count() == 1 ? "y" : "ies", pack_dir);
  raepack_free(&pack);
  return 0;
}

static int cmd_update(int argc, char** argv) {
  const char* only = NULL;
  for (int i = 0; i < argc; ++i) {
    if (argv[i][0] == '-') { fprintf(stderr, "error: unknown update option '%s'\n", argv[i]); return 1; }
    if (!only) only = argv[i];
    else { fprintf(stderr, "error: unexpected argument '%s'\n", argv[i]); return 1; }
  }
  char pack_path[PATH_MAX], pack_dir[PATH_MAX];
  if (!packages_find_pack(pack_path, pack_dir)) return 1;
  rae_deps_set_update(only);  /* re-resolve: ignore lock pins, re-fetch remotes */
  RaePack pack;
  if (!packages_resolve(pack_path, pack_dir, &pack)) return 1;
  char version[160];
  build_compiler_version_string(version, sizeof(version));
  write_rae_lock(pack_path, version);
  if (only) printf("updated '%s'; rewrote %s/rae.lock\n", only, pack_dir);
  else printf("updated %d dependenc%s; rewrote %s/rae.lock\n",
              rae_deps_count(), rae_deps_count() == 1 ? "y" : "ies", pack_dir);
  raepack_free(&pack);
  return 0;
}

static int cmd_tree(void) {
  char pack_path[PATH_MAX], pack_dir[PATH_MAX];
  if (!packages_find_pack(pack_path, pack_dir)) return 1;
  RaePack pack;
  if (!packages_resolve(pack_path, pack_dir, &pack)) return 1;
  char label[256];
  if (pack.name.len > 0) snprintf(label, sizeof(label), "%.*s", (int)pack.name.len, pack.name.data);
  else snprintf(label, sizeof(label), "(project)");
  rae_deps_print_tree(stdout, label, compiler_stdlib_dir());
  raepack_free(&pack);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  if (argv[0]) {
    char resolved[PATH_MAX];
    if (realpath(argv[0], resolved)) {
      strncpy(g_rae_executable_path, resolved, sizeof(g_rae_executable_path) - 1);
    } else {
      strncpy(g_rae_executable_path, argv[0], sizeof(g_rae_executable_path) - 1);
    }
  }

  const char* cmd = argv[1];
  if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
    bool json = (argc >= 3 && strcmp(argv[2], "--json") == 0);
    print_version(json);
    return 0;
  }
  if ((strcmp(cmd, "lex") == 0 || strcmp(cmd, "parse") == 0 || strcmp(cmd, "format") == 0 ||
       strcmp(cmd, "run") == 0 || strcmp(cmd, "pack") == 0 || strcmp(cmd, "build") == 0 ||
       strcmp(cmd, "watch") == 0)) {
    return run_command(argv[1], argc - 2, argv + 2);
  }
  if (strcmp(cmd, "init") == 0) {
    return cmd_init(argc - 2, argv + 2);
  }
  if (strcmp(cmd, "toolchain") == 0) {
    return cmd_toolchain(argc - 2, argv + 2);
  }
  if (strcmp(cmd, "add") == 0) {
    return cmd_add(argc - 2, argv + 2);
  }
  if (strcmp(cmd, "fetch") == 0) {
    return cmd_fetch();
  }
  if (strcmp(cmd, "update") == 0) {
    return cmd_update(argc - 2, argv + 2);
  }
  if (strcmp(cmd, "tree") == 0) {
    return cmd_tree();
  }
  if (strcmp(cmd, "bindgen") == 0) {
    return bindgen_run(argc - 2, argv + 2);
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

/* #919: the build's format preflight rewrote these files (listed in the
 * emit subprocess's `<c>.formatted` sidecar); re-stamp their mtimes and their
 * directories' so the watcher does not treat its own writes as an edit. */
static void watch_state_absorb_formatted(WatchState* state, const char* build_dir) {
  char list_path[PATH_MAX];
  snprintf(list_path, sizeof(list_path), "%s/app.c.formatted", build_dir);
  FILE* f = fopen(list_path, "r");
  if (!f) return;
  char line[PATH_MAX];
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
    if (n == 0) continue;
    char resolved[PATH_MAX];
    const char* written = realpath(line, resolved) ? resolved : line;
    for (size_t i = 0; i < state->sources.file_count; ++i) {
      char candidate[PATH_MAX];
      const char* watched = realpath(state->sources.files[i], candidate) ? candidate : state->sources.files[i];
      if (strcmp(watched, written) == 0) {
        time_t modified = file_last_modified(state->sources.files[i]);
        state->file_mtimes[i] = (modified == (time_t)-1) ? 0 : modified;
      }
    }
    /* the atomic rename touched the containing directory too */
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", written);
    char* slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    for (size_t i = 0; i < state->sources.dir_count; ++i) {
      char candidate[PATH_MAX];
      const char* watched = realpath(state->sources.dirs[i], candidate) ? candidate : state->sources.dirs[i];
      if (strcmp(watched, dir) == 0) {
        time_t modified = file_last_modified(state->sources.dirs[i]);
        state->dir_mtimes[i] = (modified == (time_t)-1) ? 0 : modified;
      }
    }
    if (state->fallback_path) {
      char candidate[PATH_MAX];
      const char* watched = realpath(state->fallback_path, candidate) ? candidate : state->fallback_path;
      if (strcmp(watched, written) == 0) {
        time_t modified = file_last_modified(state->fallback_path);
        state->fallback_mtime = (modified == (time_t)-1) ? 0 : modified;
      }
    }
  }
  fclose(f);
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
