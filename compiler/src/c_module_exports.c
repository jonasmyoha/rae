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

void wasm_module_exports_register(CompilerContext* ctx) {
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
  fprintf(text, "],\"requests\":[]}");
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
  if (ok) emit_interface_section(ctx, out);
  return ok;
}
