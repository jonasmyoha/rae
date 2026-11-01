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
 *   A String result is one i64: the address in the high 32 bits, the length
 *     in the low 32; the host reads the bytes and frees the address with
 *     `raeModuleFree` (0 address for an empty string).
 *
 * Every wrapper first runs the runtime's set-up once (what `main` does for a
 * program: the arguments, the scheduler), and keeps the String pool balanced
 * around the call. Other types and parameter modes are refused with a
 * diagnostic: they wait for typed interfaces (Rae #129). */
#include <stdio.h>
#include <string.h>

#include "c_module_exports.h"
#include "c_decl_index.h"
#include "diag.h"
#include "mangler.h"
#include "sema.h"

const char* g_wasm_module_exports = NULL;

const char* core_function_mangled(CompilerContext* ctx, const char* name);

typedef enum { EXPORT_INT, EXPORT_FLOAT, EXPORT_FLOAT64, EXPORT_BOOL, EXPORT_STRING, EXPORT_NONE, EXPORT_BAD } ExportKind;

static ExportKind export_kind(const AstTypeRef* type) {
  if (!type) return EXPORT_NONE;
  if (type->is_opt || type->generic_args) return EXPORT_BAD;
  Str base = get_base_type_name(type);
  if (str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int64")) return EXPORT_INT;
  if (str_eq_cstr(base, "Float")) return EXPORT_FLOAT;
  if (str_eq_cstr(base, "Float64")) return EXPORT_FLOAT64;
  if (str_eq_cstr(base, "Bool")) return EXPORT_BOOL;
  if (str_eq_cstr(base, "String")) return EXPORT_STRING;
  return EXPORT_BAD;
}

static const char* export_c_type(ExportKind kind) {
  switch (kind) {
    case EXPORT_INT: return "int64_t";
    case EXPORT_FLOAT: return "float";
    case EXPORT_FLOAT64: return "double";
    case EXPORT_BOOL: return "int32_t";
    case EXPORT_STRING: return "int64_t";
    default: return "void";
  }
}

/* The one function named `name` that can be exported, or NULL (reported) */
static const AstFuncDecl* find_export(CompilerContext* ctx, const char* name) {
  size_t count = 0;
  const size_t* named = decl_index_functions(ctx, str_from_cstr(name), &count);
  const AstFuncDecl* found = NULL;
  for (size_t k = 0; k < count; k++) {
    const AstFuncDecl* fd = &ctx->all_decls[named[k]]->as.func_decl;
    if (fd->generic_params || fd->is_extern) continue;
    if (found) {
      fprintf(stderr, "error: wasm module export '%s' names more than one function\n", name);
      return NULL;
    }
    found = fd;
  }
  if (!found) fprintf(stderr, "error: wasm module export '%s' is not a function of the program\n", name);
  return found;
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
}

static bool emit_wrapper(CompilerContext* ctx, FILE* out, const char* name, const AstFuncDecl* f) {
  ExportKind result = EXPORT_NONE;
  if (f->returns) {
    if (f->returns->next) {
      fprintf(stderr, "error: wasm module export '%s' returns more than one value\n", name);
      return false;
    }
    result = export_kind(f->returns->type);
  }
  if (result == EXPORT_BAD) {
    fprintf(stderr, "error: wasm module export '%s' returns a type a module cannot export yet "
                    "(Int, Float, Float64, Bool, String)\n", name);
    return false;
  }
  for (const AstParam* p = f->params; p; p = p->next) {
    ExportKind kind = export_kind(p->type);
    if (kind == EXPORT_BAD || kind == EXPORT_NONE || p->type->is_mod) {
      fprintf(stderr, "error: wasm module export '%s': parameter '%.*s' is not an Int, Float, Float64, "
                      "Bool or String passed by view or copy\n", name, (int)p->name.len, p->name.data);
      return false;
    }
    if (kind == EXPORT_STRING && !p->type->is_view) {
      fprintf(stderr, "error: wasm module export '%s': String parameter '%.*s' must be `view`\n",
              name, (int)p->name.len, p->name.data);
      return false;
    }
  }
  const char* mangled = rae_mangle_function(ctx, f);
  fprintf(out, "__attribute__((export_name(\"%s\"))) %s rae_module_export_%s(", name, export_c_type(result), name);
  int index = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    if (index > 0) fprintf(out, ", ");
    ExportKind kind = export_kind(p->type);
    if (kind == EXPORT_STRING) fprintf(out, "int32_t p%d_address, int32_t p%d_length", index, index);
    else fprintf(out, "%s p%d", export_c_type(kind), index);
  }
  if (index == 0) fprintf(out, "void");
  fprintf(out, ") {\n  rae_module_init();\n  int rae_module_mark = rae_string_pool_mark();\n");
  index = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    if (export_kind(p->type) == EXPORT_STRING) {
      fprintf(out, "  rae_String p%d_text = { (uint8_t*)(intptr_t)p%d_address, p%d_length, p%d_length, 0 };\n",
              index, index, index, index);
    }
  }
  if (result == EXPORT_NONE) fprintf(out, "  ");
  else if (result == EXPORT_STRING) fprintf(out, "  rae_String answer = ");
  else if (result == EXPORT_BOOL) fprintf(out, "  int32_t answer = (int32_t)");
  else fprintf(out, "  %s answer = ", export_c_type(result));
  fprintf(out, "%s(", mangled);
  index = 0;
  for (const AstParam* p = f->params; p; p = p->next, index++) {
    if (index > 0) fprintf(out, ", ");
    ExportKind kind = export_kind(p->type);
    if (kind == EXPORT_STRING) fprintf(out, "(rae_View_String){ &p%d_text }", index);
    else if (kind == EXPORT_BOOL) fprintf(out, "p%d != 0", index);
    else fprintf(out, "p%d", index);
  }
  fprintf(out, ");\n");
  if (result == EXPORT_STRING) {
    fprintf(out, "  int64_t packed = 0;\n");
    fprintf(out, "  if (answer.len > 0) {\n");
    fprintf(out, "    uint8_t* copy = (uint8_t*)malloc((size_t)answer.len);\n");
    fprintf(out, "    if (copy) { memcpy(copy, answer.data, (size_t)answer.len);\n");
    fprintf(out, "      packed = ((int64_t)(uint32_t)(intptr_t)copy << 32) | (int64_t)(uint32_t)answer.len; }\n");
    fprintf(out, "  }\n");
    fprintf(out, "  rae_string_pool_flush(rae_module_mark);\n  return packed;\n}\n");
  } else if (result == EXPORT_NONE) {
    fprintf(out, "  rae_string_pool_flush(rae_module_mark);\n}\n");
  } else {
    fprintf(out, "  rae_string_pool_flush(rae_module_mark);\n  return answer;\n}\n");
  }
  return true;
}

bool emit_wasm_module_exports(CompilerContext* ctx, FILE* out) {
  if (!g_wasm_module_exports || !*g_wasm_module_exports) return true;
  emit_init(ctx, out);
  char names[4096];
  snprintf(names, sizeof names, "%s", g_wasm_module_exports);
  bool ok = true;
  for (char* name = strtok(names, " ,\t\n"); name; name = strtok(NULL, " ,\t\n")) {
    const AstFuncDecl* f = find_export(ctx, name);
    if (!f || !emit_wrapper(ctx, out, name, f)) ok = false;
  }
  return ok;
}
