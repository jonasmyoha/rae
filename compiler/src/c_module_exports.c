/* A WASM module's named exports (docs/wasm-modules.md): for each Rae function
 * the .raepack's wasm target lists in `exports`, a C wrapper the linker
 * exports under the function's Rae name (`export_name`), so a host calls
 * `add` or `greet` by that name. The wrapper is the ABI between the host and
 * the Rae function, and nothing more:
 *
 *   Int -> i64, Float -> f32, Float64 -> f64, Bool -> i32 (0 / 1).
 *   A `view String` parameter is two i32s, the address and the byte length of
 *     UTF-8 the host wrote into the module's memory (`raeModuleAlloc`), only
 *     read during the call.
 *   A struct, List, opt or enum parameter (`view` or `copy`) is two i32s too:
 *     the address and length of its binary encoding (docs/wasm-interfaces.md
 *     §2). The wrapper decodes it; a value that does not decode traps, with a
 *     message naming the export and the parameter on stderr.
 *   A String result is one i64: the address in the high 32 bits, the length
 *     in the low 32; the host reads the bytes and frees the address with
 *     `raeModuleFree` (0 address for an empty string). A struct, List, opt or
 *     enum result is the same i64 over its encoding.
 *
 * Every wrapper first runs the runtime's set-up once (what `main` does for a
 * program: the arguments, the scheduler), and keeps the String pool balanced
 * around the call. A `mod` parameter is refused: nothing crosses by
 * reference.
 *
 * The module also carries the custom section `rae.interface` (JSON, version
 * 1): each export's parameter and result types as their canonical
 * description and hash, so a host can check them before the first call
 * (docs/wasm-interfaces.md §3). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c_backend_internal.h"
#include "c_binary.h"
#include "c_module_exports.h"
#include "c_decl_index.h"
#include "diag.h"
#include "mangler.h"
#include "sema.h"

const char* g_wasm_module_exports = NULL;
const char* g_wasm_module_requests = NULL;
const char* g_wasm_module_imports = NULL;

/* The standard capabilities: named sets of the WASI imports a module built
 * here makes (docs/wasm-interfaces.md §4). The same table is
 * wasiMinimalCapabilities() in lib/wasm/WasiMinimal.rae; change both.
 * `runtime` carries no authority (empty arguments and environment, EBADF for
 * every file, a trap for exit) and is always granted. */
typedef struct { const char* capability; const char* import; } StandardImport;
static const StandardImport standard_imports[] = {
  { "runtime", "args_get" }, { "runtime", "args_sizes_get" },
  { "runtime", "environ_get" }, { "runtime", "environ_sizes_get" },
  { "runtime", "sched_yield" }, { "runtime", "proc_exit" },
  { "runtime", "fd_close" }, { "runtime", "fd_fdstat_get" },
  { "runtime", "fd_prestat_get" }, { "runtime", "fd_prestat_dir_name" },
  { "runtime", "fd_read" }, { "runtime", "fd_seek" },
  { "log", "fd_write" },
  { "clock", "clock_time_get" }, { "clock", "clock_res_get" },
  { "random", "random_get" },
};
#define WASI_MODULE "wasi_snapshot_preview1"

/* Whether `name` is listed in the comma / space separated `list`. */
static bool list_has(const char* list, const char* name) {
  if (!list) return false;
  size_t n = strlen(name);
  const char* p = list;
  while (*p) {
    while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n') p++;
    const char* start = p;
    while (*p && *p != ',' && *p != ' ' && *p != '\t' && *p != '\n') p++;
    if ((size_t)(p - start) == n && strncmp(start, name, n) == 0) return true;
  }
  return false;
}

const char* core_function_mangled(CompilerContext* ctx, const char* name);

typedef enum { EXPORT_INT, EXPORT_FLOAT, EXPORT_FLOAT64, EXPORT_BOOL, EXPORT_STRING, EXPORT_VALUE, EXPORT_NONE, EXPORT_BAD } ExportKind;

/* `type` without its view / mod mode: the value that crosses. */
static AstTypeRef* plain_type(CompilerContext* ctx, const AstTypeRef* type) {
  AstTypeRef* plain = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
  *plain = *type;
  plain->is_view = false;
  plain->is_mod = false;
  plain->next = NULL;
  return plain;
}

static ExportKind export_kind(CompilerContext* ctx, const AstTypeRef* type, char* why, size_t why_cap) {
  if (why_cap) why[0] = 0;
  if (!type) return EXPORT_NONE;
  if (!type->is_opt && !type->generic_args) {
    Str base = get_base_type_name(type);
    if (str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int64")) return EXPORT_INT;
    if (str_eq_cstr(base, "Float")) return EXPORT_FLOAT;
    if (str_eq_cstr(base, "Float64")) return EXPORT_FLOAT64;
    if (str_eq_cstr(base, "Bool")) return EXPORT_BOOL;
    if (str_eq_cstr(base, "String")) return EXPORT_STRING;
  }
  char message[1024];
  if (binary_type_crossable(ctx, plain_type(ctx, type), message, sizeof message)) return EXPORT_VALUE;
  if (why_cap) snprintf(why, why_cap, "%s", message);
  return EXPORT_BAD;
}

static const char* export_c_type(ExportKind kind) {
  switch (kind) {
    case EXPORT_INT: return "int64_t";
    case EXPORT_FLOAT: return "float";
    case EXPORT_FLOAT64: return "double";
    case EXPORT_BOOL: return "int32_t";
    case EXPORT_STRING: return "int64_t";
    case EXPORT_VALUE: return "int64_t";
    default: return "void";
  }
}

/* The C spelling of a value's type, as the function's signature has it. */
static char* value_c_type(CompilerContext* ctx, const AstTypeRef* type) {
  char* text = NULL;
  size_t len = 0;
  FILE* spelled = open_memstream(&text, &len);
  if (!spelled) return NULL;
  CFuncContext tctx = {0};
  tctx.compiler_ctx = ctx;
  tctx.module = ctx->current_module;
  emit_type_ref_as_c_type(&tctx, plain_type(ctx, type), spelled, false);
  fclose(spelled);
  return text;
}

/* The one function named `name` that can be exported, or NULL (reported
 * when `report`) */
static const AstFuncDecl* find_export(CompilerContext* ctx, const char* name, bool report) {
  size_t count = 0;
  const size_t* named = decl_index_functions(ctx, str_from_cstr(name), &count);
  const AstFuncDecl* found = NULL;
  for (size_t k = 0; k < count; k++) {
    const AstFuncDecl* fd = &ctx->all_decls[named[k]]->as.func_decl;
    if (fd->generic_params || fd->is_extern) continue;
    if (found) {
      if (report) fprintf(stderr, "error: wasm module export '%s' names more than one function\n", name);
      return NULL;
    }
    found = fd;
  }
  if (!found && report) fprintf(stderr, "error: wasm module export '%s' is not a function of the program\n", name);
  return found;
}

/* Each export name in turn (space- or comma-separated). */
static char* next_export_name(char* names, char** cursor) {
  return strtok_r(names, " ,\t\n", cursor);
}

/* ---- typed host imports (docs/wasm-interfaces.md §6) ------------------
 *
 * `imports: "game.describe"` on the wasm target: the `extern` function
 * `describe` becomes the WASM import `rae.describe`, in capability `game`.
 * The compiler writes the C function the extern names (its `extern("...")`
 * symbol): it encodes the arguments as an export's are (scalars directly, a
 * String or a value as an address and length), calls the raw import, and
 * decodes the answer. A String or value answer comes back in a buffer the
 * wrapper passes (address, capacity) as the last two arguments; the import
 * answers the length, or minus the length when it did not fit, and then the
 * wrapper fetches it with `rae.raeTakeAnswer(address)`, which lib/wasm
 * answers itself. So the host function runs once per call. */

typedef struct { char capability[64]; char name[128]; } ImportEntry;
#define MAX_IMPORTS 64
#define IMPORT_ANSWER_SPACE 1024

/* The pack's `capability.function` entries; false (reported) when one is not
 * written that way. */
static int import_entries(ImportEntry* entries, bool report) {
  int count = 0;
  if (!g_wasm_module_imports || !*g_wasm_module_imports) return 0;
  char list[1024];
  snprintf(list, sizeof list, "%s", g_wasm_module_imports);
  char* cursor = NULL;
  for (char* item = strtok_r(list, " ,\t\n", &cursor); item && count < MAX_IMPORTS;
       item = strtok_r(NULL, " ,\t\n", &cursor)) {
    char* dot = strchr(item, '.');
    if (!dot || dot == item || !dot[1]) {
      if (report) fprintf(stderr, "error: wasm target import '%s' is not written capability.function (\"game.describe\")\n", item);
      return -1;
    }
    *dot = 0;
    snprintf(entries[count].capability, sizeof entries[count].capability, "%s", item);
    snprintf(entries[count].name, sizeof entries[count].name, "%s", dot + 1);
    count++;
  }
  return count;
}

/* The one `extern` function named `name`, or NULL. */
static const AstFuncDecl* find_import_extern(CompilerContext* ctx, const char* name) {
  size_t count = 0;
  const size_t* named = decl_index_functions(ctx, str_from_cstr(name), &count);
  for (size_t k = 0; k < count; k++) {
    const AstFuncDecl* fd = &ctx->all_decls[named[k]]->as.func_decl;
    if (fd->is_extern && fd->extern_symbol) return fd;
  }
  return NULL;
}

static bool g_import_failed = false;

static void wasm_module_imports_register(CompilerContext* ctx);
static bool emit_import_wrappers(CompilerContext* ctx, FILE* out);

void wasm_module_exports_register(CompilerContext* ctx) {
  wasm_module_imports_register(ctx);
  if (!g_wasm_module_exports || !*g_wasm_module_exports) return;
  char names[4096];
  snprintf(names, sizeof names, "%s", g_wasm_module_exports);
  char* cursor = NULL;
  for (char* name = next_export_name(names, &cursor); name; name = next_export_name(NULL, &cursor)) {
    const AstFuncDecl* f = find_export(ctx, name, false);
    if (!f) continue;
    for (const AstParam* p = f->params; p; p = p->next) {
      if (export_kind(ctx, p->type, NULL, 0) == EXPORT_VALUE) binary_register_type(ctx, plain_type(ctx, p->type), 0);
    }
    if (f->returns && f->returns->type && export_kind(ctx, f->returns->type, NULL, 0) == EXPORT_VALUE)
      binary_register_type(ctx, plain_type(ctx, f->returns->type), 0);
  }
}

static void emit_init(CompilerContext* ctx, FILE* out) {
  fprintf(out, "\n/* ----- WASM module exports (docs/wasm-modules.md) ----- */\n");
  fprintf(out, "static int rae_module_initialized = 0;\n");
  fprintf(out, "static void rae_module_init(void) {\n");
  fprintf(out, "  if (rae_module_initialized) return;\n");
  fprintf(out, "  rae_module_initialized = 1;\n");
  fprintf(out, "  static char* rae_module_argv[] = { (char*)\"module\", NULL };\n");
  fprintf(out, "  rae_runtime_set_args(1, rae_module_argv);\n");
  if (core_function_mangled(ctx, "schedulerWorkerLoop")) {
    fprintf(out, "  rae_sched_install(rae_sched_hook_worker_loop, rae_sched_hook_task_wait, "
                 "rae_sched_hook_worker_count, rae_sched_hook_chunk_size, rae_sched_hook_wake, "
                 "rae_sched_hook_group_wait);\n");
    if (core_function_mangled(ctx, "schedulerIoLoop") && core_function_mangled(ctx, "schedulerRunReady")) {
      fprintf(out, "  rae_sched_install_io(rae_sched_hook_io_loop, rae_sched_hook_frame);\n");
    }
  }
  fprintf(out, "}\n");
  /* the host's buffers in the module's memory */
  fprintf(out, "__attribute__((export_name(\"raeModuleAlloc\"))) int32_t rae_module_alloc(int32_t size) {\n");
  fprintf(out, "  return (int32_t)(intptr_t)malloc(size > 0 ? (size_t)size : 1);\n}\n");
  fprintf(out, "__attribute__((export_name(\"raeModuleFree\"))) void rae_module_free(int32_t address) {\n");
  fprintf(out, "  free((void*)(intptr_t)address);\n}\n");
  /* a value the host sent that does not decode: say which, then trap */
  fprintf(out, "__attribute__((noreturn)) static void rae_module_trap(const char* message) {\n");
  fprintf(out, "  fputs(message, stderr); fputc('\\n', stderr); fflush(stderr); __builtin_trap();\n}\n");
}

static bool check_signature(CompilerContext* ctx, const char* name, const AstFuncDecl* f, ExportKind* result) {
  char why[1024];
  *result = EXPORT_NONE;
  if (f->returns) {
    if (f->returns->next) {
      fprintf(stderr, "error: wasm module export '%s' returns more than one value\n", name);
      return false;
    }
    *result = export_kind(ctx, f->returns->type, why, sizeof why);
  }
  if (*result == EXPORT_BAD) {
    fprintf(stderr, "error: wasm module export '%s' returns a type a module cannot export: %s\n", name, why);
    return false;
  }
  for (const AstParam* p = f->params; p; p = p->next) {
    ExportKind kind = export_kind(ctx, p->type, why, sizeof why);
    if (kind == EXPORT_BAD || kind == EXPORT_NONE || p->type->is_mod) {
      fprintf(stderr, "error: wasm module export '%s': parameter '%.*s' is not an Int, Float, Float64, Bool, "
                      "String or a struct, List, opt or enum value passed by view or copy%s%s\n",
              name, (int)p->name.len, p->name.data, why[0] ? ": " : "", why);
      return false;
    }
    if (kind == EXPORT_STRING && !p->type->is_view) {
      fprintf(stderr, "error: wasm module export '%s': String parameter '%.*s' must be `view`\n",
              name, (int)p->name.len, p->name.data);
      return false;
    }
  }
  return true;
}

/* The packed (address << 32 | length) i64 of `bytes` copied into fresh memory
 * the host frees, written as `packed`. */
static void emit_pack(FILE* out, const char* data, const char* len) {
  fprintf(out, "  int64_t packed = 0;\n");
  fprintf(out, "  if (%s > 0) {\n", len);
  fprintf(out, "    uint8_t* copy = (uint8_t*)malloc((size_t)%s);\n", len);
  fprintf(out, "    if (copy) { memcpy(copy, %s, (size_t)%s);\n", data, len);
  fprintf(out, "      packed = ((int64_t)(uint32_t)(intptr_t)copy << 32) | (int64_t)(uint32_t)%s; }\n", len);
  fprintf(out, "  }\n");
}

static bool emit_wrapper(CompilerContext* ctx, FILE* out, const char* name, const AstFuncDecl* f) {
  ExportKind result = EXPORT_NONE;
  if (!check_signature(ctx, name, f, &result)) return false;
  /* the decoders and encoders of this export's values */
  int index = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    if (export_kind(ctx, p->type, NULL, 0) != EXPORT_VALUE) continue;
    char codec[256];
    snprintf(codec, sizeof codec, "%s_p%d", name, index);
    binary_emit_value_codec(ctx, out, codec, p->type);
  }
  if (result == EXPORT_VALUE) {
    char codec[256];
    snprintf(codec, sizeof codec, "%s_result", name);
    binary_emit_value_codec(ctx, out, codec, f->returns->type);
  }
  const char* mangled = rae_mangle_function(ctx, f);
  fprintf(out, "__attribute__((export_name(\"%s\"))) %s rae_module_export_%s(", name, export_c_type(result), name);
  index = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    if (index > 0) fprintf(out, ", ");
    ExportKind kind = export_kind(ctx, p->type, NULL, 0);
    if (kind == EXPORT_STRING || kind == EXPORT_VALUE) fprintf(out, "int32_t p%d_address, int32_t p%d_length", index, index);
    else fprintf(out, "%s p%d", export_c_type(kind), index);
  }
  if (index == 0) fprintf(out, "void");
  fprintf(out, ") {\n  rae_module_init();\n  int rae_module_mark = rae_string_pool_mark();\n");
  index = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    ExportKind kind = export_kind(ctx, p->type, NULL, 0);
    if (kind == EXPORT_STRING) {
      fprintf(out, "  rae_String p%d_text = { (uint8_t*)(intptr_t)p%d_address, p%d_length, p%d_length, 0 };\n",
              index, index, index, index);
    } else if (kind == EXPORT_VALUE) {
      char* c_type = value_c_type(ctx, p->type);
      fprintf(out, "  rae_BinIn p%d_in = { (const uint8_t*)(intptr_t)p%d_address, p%d_length, 0 };\n", index, index, index);
      fprintf(out, "  %s p%d_value; memset(&p%d_value, 0, sizeof p%d_value);\n", c_type ? c_type : "int64_t", index, index, index);
      fprintf(out, "  if (p%d_length < 0 || !rae_binGetValue_%s_p%d(&p%d_in, &p%d_value)) {\n", index, name, index, index, index);
      fprintf(out, "    rae_binFreeValue_%s_p%d(&p%d_value);\n", name, index, index);
      fprintf(out, "    rae_module_trap(\"wasm module export '%s': parameter '%.*s' does not decode as %s\");\n  }\n",
              name, (int)p->name.len, p->name.data, binary_schema_text(ctx, plain_type(ctx, p->type)));
      free(c_type);
    }
  }
  char* result_c_type = result == EXPORT_VALUE ? value_c_type(ctx, f->returns->type) : NULL;
  if (result == EXPORT_NONE) fprintf(out, "  ");
  else if (result == EXPORT_STRING) fprintf(out, "  rae_String answer = ");
  else if (result == EXPORT_BOOL) fprintf(out, "  int32_t answer = (int32_t)");
  else if (result == EXPORT_VALUE) fprintf(out, "  %s answer = ", result_c_type ? result_c_type : "int64_t");
  else fprintf(out, "  %s answer = ", export_c_type(result));
  free(result_c_type);
  fprintf(out, "%s(", mangled);
  index = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    if (index > 0) fprintf(out, ", ");
    ExportKind kind = export_kind(ctx, p->type, NULL, 0);
    if (kind == EXPORT_STRING) fprintf(out, "(rae_View_String){ &p%d_text }", index);
    else if (kind == EXPORT_VALUE) fprintf(out, "%sp%d_value", p->type->is_view ? "&" : "", index);
    else if (kind == EXPORT_BOOL) fprintf(out, "p%d != 0", index);
    else fprintf(out, "p%d", index);
  }
  fprintf(out, ");\n");
  /* A `view` value was only lent to the call: free it. A `copy` one now
   * belongs to the function, which drops it. */
  index = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    if (export_kind(ctx, p->type, NULL, 0) == EXPORT_VALUE && p->type->is_view)
      fprintf(out, "  rae_binFreeValue_%s_p%d(&p%d_value);\n", name, index, index);
  }
  if (result == EXPORT_STRING) {
    emit_pack(out, "answer.data", "answer.len");
    fprintf(out, "  rae_string_pool_flush(rae_module_mark);\n  return packed;\n}\n");
  } else if (result == EXPORT_VALUE) {
    fprintf(out, "  rae_BinOut answer_out = {0};\n");
    fprintf(out, "  rae_binPutValue_%s_result(&answer_out, &answer);\n", name);
    fprintf(out, "  rae_binFreeValue_%s_result(&answer);\n", name);
    fprintf(out, "  if (answer_out.failed) { rae_ext_rae_buf_free(answer_out.data); "
                 "rae_module_trap(\"wasm module export '%s': the result is too large to encode\"); }\n", name);
    emit_pack(out, "answer_out.data", "answer_out.len");
    fprintf(out, "  rae_ext_rae_buf_free(answer_out.data);\n");
    fprintf(out, "  rae_string_pool_flush(rae_module_mark);\n  return packed;\n}\n");
  } else if (result == EXPORT_NONE) {
    fprintf(out, "  rae_string_pool_flush(rae_module_mark);\n}\n");
  } else {
    fprintf(out, "  rae_string_pool_flush(rae_module_mark);\n  return answer;\n}\n");
  }
  return true;
}

/* ---- the `rae.interface` custom section -------------------------------- */

typedef struct { char* data; size_t len; FILE* out; } JsonText;

/* `"type":"<description>","hash":"<16 hex digits>"` */
static void json_type(CompilerContext* ctx, FILE* out, const AstTypeRef* type) {
  const char* text = binary_schema_text(ctx, plain_type(ctx, type));
  fprintf(out, "\"type\":\"%s\",\"hash\":\"%016llx\"", text,
          (unsigned long long)(uint64_t)binary_schema_hash(text));
}

/* The section as an assembler directive: a C `section` attribute does not
 * make a WASM custom section (it stays a data segment), `.section
 * .custom_section.<name>` does. The JSON goes through the C name restore, so
 * a field renamed for C keeps its Rae name. */
static void emit_interface_section(CompilerContext* ctx, FILE* out) {
  char* json = NULL;
  size_t json_len = 0;
  FILE* text = open_memstream(&json, &json_len);
  if (!text) return;
  fprintf(text, "{\"version\":1,\"exports\":[");
  char names[4096];
  snprintf(names, sizeof names, "%s", g_wasm_module_exports);
  char* cursor = NULL;
  bool first = true;
  for (char* name = next_export_name(names, &cursor); name; name = next_export_name(NULL, &cursor)) {
    const AstFuncDecl* f = find_export(ctx, name, false);
    if (!f) continue;
    fprintf(text, "%s{\"name\":\"%s\",\"params\":[", first ? "" : ",", name);
    first = false;
    for (const AstParam* p = f->params; p; p = p->next) {
      fprintf(text, "{\"name\":\"%.*s\",", (int)p->name.len, p->name.data);
      json_type(ctx, text, p->type);
      fprintf(text, "}%s", p->next ? "," : "");
    }
    fprintf(text, "],\"result\":");
    if (f->returns && f->returns->type) { fprintf(text, "{"); json_type(ctx, text, f->returns->type); fprintf(text, "}"); }
    else fprintf(text, "null");
    fprintf(text, "}");
  }
  fprintf(text, "],\"imports\":[");
  {
    ImportEntry entries[MAX_IMPORTS];
    int count = import_entries(entries, false);
    bool first_import = true;
    for (int i = 0; i < count; i++) {
      const AstFuncDecl* f = find_import_extern(ctx, entries[i].name);
      if (!f) continue;
      fprintf(text, "%s{\"name\":\"%s\",\"capability\":\"%s\",\"params\":[", first_import ? "" : ",",
              entries[i].name, entries[i].capability);
      first_import = false;
      for (const AstParam* p = f->params; p; p = p->next) {
        fprintf(text, "{\"name\":\"%.*s\",", (int)p->name.len, p->name.data);
        json_type(ctx, text, p->type);
        fprintf(text, "}%s", p->next ? "," : "");
      }
      fprintf(text, "],\"result\":");
      if (f->returns && f->returns->type) { fprintf(text, "{"); json_type(ctx, text, f->returns->type); fprintf(text, "}"); }
      else fprintf(text, "null");
      fprintf(text, "}");
    }
  }
  fprintf(text, "],\"requests\":[");
  if (g_wasm_module_requests) {
    char requests[1024];
    snprintf(requests, sizeof requests, "%s", g_wasm_module_requests);
    char* request_cursor = NULL;
    bool first_request = true;
    for (char* request = strtok_r(requests, " ,\t\n", &request_cursor); request;
         request = strtok_r(NULL, " ,\t\n", &request_cursor)) {
      fprintf(text, "%s\"%s\"", first_request ? "" : ",", request);
      first_request = false;
    }
  }
  fprintf(text, "]}");
  fclose(text);
  /* As a C string literal: the JSON's quotes escaped. */
  char* literal = NULL;
  size_t literal_len = 0;
  FILE* c = open_memstream(&literal, &literal_len);
  if (!c) { free(json); return; }
  /* `.ascii "<json>"` inside a C string: a JSON quote is `\"` for the
   * assembler, and that is `\\\"` in the C literal. */
  fprintf(c, "__asm__(\".section .custom_section.rae.interface,\\\"\\\",@\\n.ascii \\\"");
  for (size_t i = 0; i < json_len; i++) {
    if (json[i] == '"') fputs("\\\\\\\"", c);
    else if (json[i] == '\\') fputs("\\\\\\\\", c);
    else fputc(json[i], c);
  }
  fprintf(c, "\\\"\\n.text\\n\");\n");
  fclose(c);
  c_restore_names_in_literals(literal, literal_len, out);
  free(literal);
  free(json);
}

bool emit_wasm_module_exports(CompilerContext* ctx, FILE* out) {
  if (!g_wasm_module_exports || !*g_wasm_module_exports) return true;
  emit_init(ctx, out);
  char names[4096];
  snprintf(names, sizeof names, "%s", g_wasm_module_exports);
  char* cursor = NULL;
  bool ok = true;
  for (char* name = next_export_name(names, &cursor); name; name = next_export_name(NULL, &cursor)) {
    const AstFuncDecl* f = find_export(ctx, name, true);
    if (!f || !emit_wrapper(ctx, out, name, f)) ok = false;
  }
  if (!emit_import_wrappers(ctx, out)) ok = false;
  if (ok) emit_interface_section(ctx, out);
  return ok;
}

/* ---- the import check after the link ------------------------------------ */

typedef struct { const uint8_t* data; size_t len; size_t pos; bool ok; } WasmBytes;

static uint32_t read_leb(WasmBytes* b) {
  uint32_t value = 0;
  for (int shift = 0; shift < 35; shift += 7) {
    if (b->pos >= b->len) { b->ok = false; return 0; }
    uint8_t byte = b->data[b->pos++];
    value |= (uint32_t)(byte & 0x7f) << shift;
    if (!(byte & 0x80)) return value;
  }
  b->ok = false;
  return 0;
}

static void skip_limits(WasmBytes* b) {
  if (b->pos >= b->len) { b->ok = false; return; }
  uint8_t flags = b->data[b->pos++];
  read_leb(b);
  if (flags & 1) read_leb(b);
}

/* The capability that covers WASI import `name`, or NULL. */
static const char* standard_capability(const char* name) {
  for (size_t i = 0; i < sizeof standard_imports / sizeof standard_imports[0]; i++)
    if (strcmp(standard_imports[i].import, name) == 0) return standard_imports[i].capability;
  return NULL;
}

bool wasm_module_check_imports(const char* wasm_path) {
  FILE* f = fopen(wasm_path, "rb");
  if (!f) { fprintf(stderr, "error: cannot read the built module %s\n", wasm_path); return false; }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* data = size > 0 ? (uint8_t*)malloc((size_t)size) : NULL;
  if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
    fclose(f); free(data);
    fprintf(stderr, "error: cannot read the built module %s\n", wasm_path);
    return false;
  }
  fclose(f);
  WasmBytes b = { data, (size_t)size, 8, true };
  bool ok = size >= 8 && memcmp(data, "\0asm", 4) == 0;
  int refused = 0;
  while (ok && b.ok && b.pos < b.len) {
    uint8_t id = b.data[b.pos++];
    uint32_t section_size = read_leb(&b);
    size_t end = b.pos + section_size;
    if (!b.ok || end > b.len) { ok = false; break; }
    if (id == 2) {
      uint32_t count = read_leb(&b);
      for (uint32_t i = 0; i < count && b.ok; i++) {
        uint32_t module_len = read_leb(&b);
        if (!b.ok || b.pos + module_len > end) { b.ok = false; break; }
        char module_name[128];
        snprintf(module_name, sizeof module_name, "%.*s", (int)module_len, (const char*)b.data + b.pos);
        b.pos += module_len;
        uint32_t name_len = read_leb(&b);
        if (!b.ok || b.pos + name_len > end) { b.ok = false; break; }
        char name[128];
        snprintf(name, sizeof name, "%.*s", (int)name_len, (const char*)b.data + b.pos);
        b.pos += name_len;
        if (b.pos >= end) { b.ok = false; break; }
        uint8_t kind = b.data[b.pos++];
        if (kind == 0) read_leb(&b);
        else if (kind == 1) { b.pos++; skip_limits(&b); }
        else if (kind == 2) skip_limits(&b);
        else if (kind == 3) b.pos += 2;
        else if (kind == 4) { b.pos++; read_leb(&b); }
        else { b.ok = false; break; }
        const char* capability = strcmp(module_name, WASI_MODULE) == 0 ? standard_capability(name) : NULL;
        if (strcmp(module_name, "rae") == 0) {
          /* lib/wasm's own answer fetch, and the typed imports' capabilities */
          if (strcmp(name, "raeTakeAnswer") == 0) continue;
          ImportEntry entries[MAX_IMPORTS];
          int entry_count = import_entries(entries, false);
          for (int e = 0; e < entry_count; e++)
            if (strcmp(entries[e].name, name) == 0) capability = entries[e].capability;
        }
        if (capability && (strcmp(capability, "runtime") == 0 || list_has(g_wasm_module_requests, capability))) continue;
        if (refused++ == 0)
          fprintf(stderr, "error: the module imports what its wasm target's `requests` do not cover (docs/wasm-interfaces.md §4):\n");
        if (capability)
          fprintf(stderr, "  %s.%s: add `%s` to the requests\n", module_name, name, capability);
        else
          fprintf(stderr, "  %s.%s: no capability covers it\n", module_name, name);
      }
    }
    b.pos = end;
  }
  free(data);
  if (!ok || !b.ok) {
    fprintf(stderr, "error: %s is not a WASM module this check can read\n", wasm_path);
    return false;
  }
  return refused == 0;
}


/* ---- typed host imports: register, check, prototypes, wrappers ------------ */

static void wasm_module_imports_register(CompilerContext* ctx) {
  ImportEntry entries[MAX_IMPORTS];
  int count = import_entries(entries, false);
  for (int i = 0; i < count; i++) {
    const AstFuncDecl* f = find_import_extern(ctx, entries[i].name);
    if (!f) continue;
    for (const AstParam* p = f->params; p; p = p->next) {
      ExportKind kind = export_kind(ctx, p->type, NULL, 0);
      if (kind == EXPORT_VALUE || kind == EXPORT_STRING) binary_register_type(ctx, plain_type(ctx, p->type), 0);
    }
    if (f->returns && f->returns->type) {
      ExportKind kind = export_kind(ctx, f->returns->type, NULL, 0);
      if (kind == EXPORT_VALUE || kind == EXPORT_STRING) binary_register_type(ctx, plain_type(ctx, f->returns->type), 0);
    }
  }
}

/* Whether the import's types can cross; false after saying why. */
static bool import_signature_ok(CompilerContext* ctx, const char* name, const AstFuncDecl* f, ExportKind* result) {
  char why[1024];
  *result = EXPORT_NONE;
  if (f->returns) {
    if (f->returns->next) {
      fprintf(stderr, "error: wasm module import '%s' returns more than one value\n", name);
      return false;
    }
    *result = export_kind(ctx, f->returns->type, why, sizeof why);
    if (*result == EXPORT_BAD) {
      fprintf(stderr, "error: wasm module import '%s' returns a type that cannot cross: %s\n", name, why);
      return false;
    }
  }
  for (const AstParam* p = f->params; p; p = p->next) {
    ExportKind kind = export_kind(ctx, p->type, why, sizeof why);
    bool by_view = p->type->is_view;
    if (kind == EXPORT_BAD || kind == EXPORT_NONE || p->type->is_mod
        || ((kind == EXPORT_STRING || kind == EXPORT_VALUE) && !by_view)) {
      fprintf(stderr, "error: wasm module import '%s': parameter '%.*s' must be an Int, Float, Float64 or Bool, "
                      "or a String or a struct, List, opt or enum value passed by view%s%s\n",
              name, (int)p->name.len, p->name.data, why[0] ? ": " : "", why);
      return false;
    }
  }
  return true;
}

static const char* import_scalar_c(ExportKind kind) {
  switch (kind) {
    case EXPORT_INT: return "int64_t";
    case EXPORT_FLOAT: return "float";
    case EXPORT_FLOAT64: return "double";
    case EXPORT_BOOL: return "rae_Bool";
    default: return "void";
  }
}

/* `<result> <symbol>(<params>)`: the extern's C signature, as its call sites
 * pass the arguments. */
static void emit_import_signature(CompilerContext* ctx, FILE* out, const AstFuncDecl* f, ExportKind result) {
  if (result == EXPORT_STRING) fprintf(out, "rae_String ");
  else if (result == EXPORT_VALUE) { char* c = value_c_type(ctx, f->returns->type); fprintf(out, "%s ", c ? c : "int64_t"); free(c); }
  else fprintf(out, "%s ", import_scalar_c(result));
  fprintf(out, "%s(", f->extern_symbol);
  int index = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    if (index > 0) fprintf(out, ", ");
    ExportKind kind = export_kind(ctx, p->type, NULL, 0);
    if (kind == EXPORT_STRING) fprintf(out, "rae_View_String p%d", index);
    else if (kind == EXPORT_VALUE) { char* c = value_c_type(ctx, p->type); fprintf(out, "const %s* p%d", c ? c : "int64_t", index); free(c); }
    else fprintf(out, "%s p%d", import_scalar_c(kind), index);
  }
  if (index == 0) fprintf(out, "void");
  fprintf(out, ")");
}

void wasm_module_imports_prototypes(CompilerContext* ctx, FILE* out) {
  ImportEntry entries[MAX_IMPORTS];
  int count = import_entries(entries, true);
  if (count < 0) { g_import_failed = true; return; }
  for (int i = 0; i < count; i++) {
    const AstFuncDecl* f = find_import_extern(ctx, entries[i].name);
    if (!f) {
      fprintf(stderr, "error: wasm target import '%s.%s' names no `extern` function of the program\n",
              entries[i].capability, entries[i].name);
      g_import_failed = true;
      continue;
    }
    ExportKind result;
    if (!import_signature_ok(ctx, entries[i].name, f, &result)) { g_import_failed = true; continue; }
    emit_import_signature(ctx, out, f, result);
    fprintf(out, ";\n");
  }
}

static void emit_import_wrapper(CompilerContext* ctx, FILE* out, const ImportEntry* entry, const AstFuncDecl* f) {
  ExportKind result;
  if (!import_signature_ok(ctx, entry->name, f, &result)) return;
  const char* name = entry->name;
  bool answer_in_memory = result == EXPORT_STRING || result == EXPORT_VALUE;
  /* the codecs of its values */
  int index = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    if (export_kind(ctx, p->type, NULL, 0) != EXPORT_VALUE) continue;
    char codec[256];
    snprintf(codec, sizeof codec, "%s_import_p%d", name, index);
    binary_emit_value_codec(ctx, out, codec, p->type);
  }
  if (result == EXPORT_VALUE) {
    char codec[256];
    snprintf(codec, sizeof codec, "%s_import_result", name);
    binary_emit_value_codec(ctx, out, codec, f->returns->type);
  }
  /* the raw WASM import */
  const char* raw_result = result == EXPORT_BOOL ? "int32_t" : answer_in_memory ? "int64_t" : import_scalar_c(result);
  fprintf(out, "__attribute__((import_module(\"rae\"), import_name(\"%s\"))) %s rae_import_raw_%s(", name, raw_result, name);
  index = 0;
  int raw_count = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    ExportKind kind = export_kind(ctx, p->type, NULL, 0);
    if (kind == EXPORT_STRING || kind == EXPORT_VALUE) fprintf(out, "%sint32_t, int32_t", raw_count++ ? ", " : "");
    else if (kind == EXPORT_BOOL) fprintf(out, "%sint32_t", raw_count++ ? ", " : "");
    else fprintf(out, "%s%s", raw_count++ ? ", " : "", import_scalar_c(kind));
  }
  if (answer_in_memory) fprintf(out, "%sint32_t, int32_t", raw_count++ ? ", " : "");
  if (raw_count == 0) fprintf(out, "void");
  fprintf(out, ");\n");
  /* the wrapper the extern's call sites call */
  emit_import_signature(ctx, out, f, result);
  fprintf(out, " {\n");
  index = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    if (export_kind(ctx, p->type, NULL, 0) != EXPORT_VALUE) continue;
    fprintf(out, "  rae_BinOut p%d_out = {0}; rae_binPutValue_%s_import_p%d(&p%d_out, p%d);\n", index, name, index, index, index);
    fprintf(out, "  if (p%d_out.failed) rae_module_trap(\"host import '%s': parameter '%.*s' is too large to encode\");\n",
            index, name, (int)p->name.len, p->name.data);
  }
  if (answer_in_memory) fprintf(out, "  uint8_t answer_space[%d];\n", IMPORT_ANSWER_SPACE);
  if (result == EXPORT_NONE) fprintf(out, "  rae_import_raw_%s(", name);
  else fprintf(out, "  %s raw = rae_import_raw_%s(", raw_result, name);
  index = 0;
  raw_count = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    ExportKind kind = export_kind(ctx, p->type, NULL, 0);
    if (raw_count++) fprintf(out, ", ");
    if (kind == EXPORT_STRING) fprintf(out, "(int32_t)(intptr_t)p%d.ptr->data, (int32_t)p%d.ptr->len", index, index);
    else if (kind == EXPORT_VALUE) fprintf(out, "(int32_t)(intptr_t)p%d_out.data, (int32_t)p%d_out.len", index, index);
    else if (kind == EXPORT_BOOL) fprintf(out, "p%d ? 1 : 0", index);
    else fprintf(out, "p%d", index);
  }
  if (answer_in_memory) fprintf(out, "%s(int32_t)(intptr_t)answer_space, %d", raw_count++ ? ", " : "", IMPORT_ANSWER_SPACE);
  fprintf(out, ");\n");
  index = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    if (export_kind(ctx, p->type, NULL, 0) == EXPORT_VALUE) fprintf(out, "  rae_ext_rae_buf_free(p%d_out.data);\n", index);
  }
  if (result == EXPORT_NONE) { fprintf(out, "}\n"); return; }
  if (!answer_in_memory) {
    if (result == EXPORT_BOOL) fprintf(out, "  return raw != 0;\n}\n");
    else fprintf(out, "  return raw;\n}\n");
    return;
  }
  /* the answer: in answer_space, or fetched when it did not fit */
  fprintf(out, "  const uint8_t* answer = answer_space; uint8_t* big = NULL; int64_t answer_length = raw;\n");
  fprintf(out, "  if (raw > %d) rae_module_trap(\"host import '%s' answered more than its buffer\");\n", IMPORT_ANSWER_SPACE, name);
  fprintf(out, "  if (raw < 0) {\n");
  fprintf(out, "    big = (uint8_t*)malloc((size_t)-raw);\n");
  fprintf(out, "    if (!big) rae_module_trap(\"host import '%s': no memory for its answer\");\n", name);
  fprintf(out, "    answer_length = rae_import_take_answer((int32_t)(intptr_t)big);\n");
  fprintf(out, "    if (answer_length != -raw) { free(big); rae_module_trap(\"host import '%s': its answer did not arrive\"); }\n", name);
  fprintf(out, "    answer = big;\n  }\n");
  if (result == EXPORT_STRING) {
    fprintf(out, "  if (!rae_bin_utf8(answer, answer_length)) { free(big); rae_module_trap(\"host import '%s' answered text that is not UTF-8\"); }\n", name);
    fprintf(out, "  rae_String text = answer_length > 0 ? rae_ext_rae_str_from_bytes((uint8_t*)answer, 0, answer_length) : (rae_String){0};\n");
    fprintf(out, "  free(big);\n  return text;\n}\n");
    return;
  }
  char* c = value_c_type(ctx, f->returns->type);
  fprintf(out, "  rae_BinIn in = { answer, answer_length, 0 };\n");
  fprintf(out, "  %s value; memset(&value, 0, sizeof value);\n", c ? c : "int64_t");
  fprintf(out, "  if (!rae_binGetValue_%s_import_result(&in, &value)) {\n", name);
  fprintf(out, "    rae_binFreeValue_%s_import_result(&value); free(big);\n", name);
  fprintf(out, "    rae_module_trap(\"host import '%s' answered a value that does not decode as %s\");\n  }\n",
          name, binary_schema_text(ctx, plain_type(ctx, f->returns->type)));
  fprintf(out, "  free(big);\n  return value;\n}\n");
  free(c);
}

static bool emit_import_wrappers(CompilerContext* ctx, FILE* out) {
  ImportEntry entries[MAX_IMPORTS];
  int count = import_entries(entries, false);
  if (count <= 0) return !g_import_failed;
  fprintf(out, "__attribute__((import_module(\"rae\"), import_name(\"raeTakeAnswer\"))) "
               "int64_t rae_import_take_answer(int32_t address);\n");
  for (int i = 0; i < count; i++) {
    const AstFuncDecl* f = find_import_extern(ctx, entries[i].name);
    if (f) emit_import_wrapper(ctx, out, &entries[i], f);
  }
  return !g_import_failed;
}
