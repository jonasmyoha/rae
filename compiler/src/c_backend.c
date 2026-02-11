#include "c_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vm_registry.h"
#include "lexer.h"
#include "diag.h"

typedef struct {
  int64_t next;
} TickCounter;

// Precedence levels for C expressions
// Based on C operator precedence
enum {
  PREC_LOWEST = 0,
  PREC_COMMA,
  PREC_ASSIGN,
  PREC_TERNARY,
  PREC_LOGICAL_OR,
  PREC_LOGICAL_AND,
  PREC_BITWISE_OR,
  PREC_BITWISE_XOR,
  PREC_BITWISE_AND,
  PREC_EQUALITY,    // == !=
  PREC_RELATIONAL,  // < > <= >=
  PREC_SHIFT,
  PREC_ADD,         // + -
  PREC_MUL,         // * / %
  PREC_UNARY,       // ! - + * &
  PREC_CALL,        // () [] . ->
  PREC_ATOMIC
};

typedef struct {
  const struct AstBlock* block;
  int scope_depth;
} CDeferEntry;

typedef struct {
  CDeferEntry entries[64];
  int count;
} CDeferStack;

typedef struct {
  const AstModule* module;
  const AstFuncDecl* func_decl;
  const AstParam* params;
  const AstIdentifierPart* generic_params;
  const char* return_type_name;
  Str locals[256];
  Str local_types[256];
  const AstTypeRef* local_type_refs[256];
  bool local_is_ptr[256];
  bool local_is_mod[256];
  size_t local_count;
  bool returns_value;
  size_t temp_counter;
  const AstTypeRef* expected_type;
  const struct VmRegistry* registry;
  bool uses_raylib;
  bool is_main;
  int scope_depth;
  CDeferStack defer_stack;
} CFuncContext;

// Forward declarations
static const char* find_raylib_mapping(Str name);
static const AstDecl* find_type_decl(const AstModule* module, Str name);
static const AstDecl* find_enum_decl(const AstModule* module, Str name);
static bool has_property(const AstProperty* props, const char* name);
static bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out, int parent_prec, bool is_lvalue);
static bool emit_function(const AstModule* module, const AstFuncDecl* func, FILE* out, const struct VmRegistry* registry, bool uses_raylib);
static bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_spawn_wrapper(CFuncContext* ctx, const AstFuncDecl* func, FILE* out);
static bool emit_defers(CFuncContext* ctx, int min_depth, FILE* out);
static void pop_defers(CFuncContext* ctx, int depth);
static bool emit_call(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_log_call(CFuncContext* ctx, const AstExpr* expr, FILE* out, bool newline);
static bool emit_call_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_string_literal(FILE* out, Str literal);
static bool emit_param_list(CFuncContext* ctx, const AstParam* params, FILE* out, bool is_extern);
static const char* map_rae_type_to_c(Str type_name);
static bool is_primitive_type(Str type_name);
static bool is_raylib_builtin_type(Str type_name);
static bool is_pointer_type(CFuncContext* ctx, Str name);
static bool is_mod_type(CFuncContext* ctx, Str name);
static Str get_local_type_name(CFuncContext* ctx, Str name);
static const AstTypeRef* get_local_type_ref(CFuncContext* ctx, Str name);
static Str get_base_type_name(const AstTypeRef* type);
static void emit_mangled_function_name(const AstFuncDecl* func, FILE* out);
static const AstFuncDecl* find_function_overload(const AstModule* module, CFuncContext* ctx, Str name, const Str* param_types, uint16_t param_count);
static bool emit_type_ref_as_c_type(CFuncContext* ctx, const AstTypeRef* type, FILE* out);
static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_loop(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_match(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool is_wildcard_pattern(const AstExpr* expr);
static bool is_string_literal_expr(const AstExpr* expr);
static bool match_cases_use_string(const AstMatchCase* cases, bool* out_use_string);
static bool match_arms_use_string(const AstMatchArm* arms, bool* out_use_string);
static bool func_has_return_value(const AstFuncDecl* func);
static const char* c_return_type(CFuncContext* ctx, const AstFuncDecl* func);

static Str get_base_type_name(const AstTypeRef* type) {
    if (!type || !type->parts) return (Str){0};
    return type->parts->text;
}

static Str d_get_param_type_name(const AstParam* p) {
    if (!p || !p->type || !p->type->parts) return (Str){0};
    Str base = p->type->parts->text;
    if (p->type->is_view) {
        char* buf = malloc(base.len + 6);
        sprintf(buf, "view %.*s", (int)base.len, base.data);
        return str_from_buf(buf, strlen(buf)); // leak
    }
    if (p->type->is_mod) {
        char* buf = malloc(base.len + 5);
        sprintf(buf, "mod %.*s", (int)base.len, base.data);
        return str_from_buf(buf, strlen(buf)); // leak
    }
    return base;
}

static Str d_get_return_type_name(const AstFuncDecl* decl) {
    if (!decl || !decl->returns || !decl->returns->type) return (Str){0};
    return get_base_type_name(decl->returns->type);
}

// --- Project-Wide State ---
static const AstDecl* g_all_decls[2048];
static size_t g_all_decl_count = 0;
static const AstTypeRef* g_generic_types[512];
static size_t g_generic_type_count = 0;
static const AstTypeRef* g_emitted_generic_types[512];
static size_t g_emitted_generic_type_count = 0;

static void register_decl(const AstDecl* decl) {
    if (!decl) return;
    for (size_t i = 0; i < g_all_decl_count; i++) {
        if (g_all_decls[i] == decl) return;
    }
    if (g_all_decl_count < 2048) {
        g_all_decls[g_all_decl_count++] = decl;
    }
}

static void collect_decls_from_module(const AstModule* module) {
    if (!module) return;
    static const AstModule* visited[128];
    static size_t visited_count = 0;
    for (size_t i = 0; i < visited_count; i++) {
        if (visited[i] == module) return;
    }
    if (visited_count < 128) {
        visited[visited_count++] = module;
    }

    for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
        register_decl(decl);
    }

    for (const AstImport* imp = module->imports; imp; imp = imp->next) {
        collect_decls_from_module(imp->module);
    }
    visited_count--;
}

static bool is_generic_param(const AstTypeRef* type, const AstIdentifierPart* generic_params) {
    if (!type || !type->parts || type->generic_args) return false;
    Str name = type->parts->text;
    if (name.len == 1 && name.data[0] >= 'A' && name.data[0] <= 'Z') return true;
    const AstIdentifierPart* gp = generic_params;
    while (gp) {
        if (str_eq(gp->text, name)) return true;
        gp = gp->next;
    }
    return false;
}

static bool type_refs_equal(const AstTypeRef* a, const AstTypeRef* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    
    bool a_gen = is_generic_param(a, NULL);
    bool b_gen = is_generic_param(b, NULL);
    if (a_gen && b_gen) return true; // Both are generic params -> equivalent to Any
    if (a_gen || b_gen) {
        // One is generic, one is not. 
        // If the non-generic one is 'Any', they are equivalent.
        if (a_gen && str_eq_cstr(b->parts->text, "Any")) return true;
        if (b_gen && str_eq_cstr(a->parts->text, "Any")) return true;
        return false;
    }

    if (a->parts && b->parts) {
        if (!str_eq(a->parts->text, b->parts->text)) return false;
    } else if (a->parts != b->parts) {
        return false;
    }
    
    if (a->is_opt != b->is_opt || a->is_view != b->is_view || a->is_mod != b->is_mod) return false;
    
    const AstTypeRef* aa = a->generic_args;
    const AstTypeRef* ab = b->generic_args;
    while (aa && ab) {
        if (!type_refs_equal(aa, ab)) return false;
        aa = aa->next;
        ab = ab->next;
    }
    return aa == NULL && ab == NULL;
}

static void register_generic_type(const AstTypeRef* type) {
    if (!type || !type->generic_args) return;
    for (size_t i = 0; i < g_generic_type_count; i++) {
        if (type_refs_equal(g_generic_types[i], type)) return;
    }
    if (g_generic_type_count < 512) {
        g_generic_types[g_generic_type_count++] = type;
    }
    
    // Recursively register generic arguments
    for (const AstTypeRef* a = type->generic_args; a; a = a->next) {
        register_generic_type(a);
    }
}

static void collect_type_refs_expr(const AstExpr* e);
static void collect_type_refs_stmt(const AstStmt* s);

static void collect_type_refs_expr(const AstExpr* e) {
    if (!e) return;
    if (e->kind == AST_EXPR_OBJECT && e->as.object_literal.type) {
        register_generic_type(e->as.object_literal.type);
    }
    if (e->kind == AST_EXPR_CALL) {
        collect_type_refs_expr(e->as.call.callee);
        for (const AstCallArg* arg = e->as.call.args; arg; arg = arg->next) {
            collect_type_refs_expr(arg->value);
        }
    }
}

static void collect_type_refs_stmt(const AstStmt* s) {
    if (!s) return;
    if (s->kind == AST_STMT_LET && s->as.let_stmt.type) {
        register_generic_type(s->as.let_stmt.type);
    }
    if (s->kind == AST_STMT_EXPR) {
        collect_type_refs_expr(s->as.expr_stmt);
    }
}

static void collect_type_refs_module(void) {
    for (size_t i = 0; i < g_all_decl_count; i++) {
        const AstDecl* d = g_all_decls[i];
        if (d->kind == AST_DECL_FUNC) {
            if (d->as.func_decl.returns && d->as.func_decl.returns->type) {
                register_generic_type(d->as.func_decl.returns->type);
            }
            if (d->as.func_decl.body) {
                for (const AstStmt* s = d->as.func_decl.body->first; s; s = s->next) {
                    collect_type_refs_stmt(s);
                }
            }
        } else if (d->kind == AST_DECL_TYPE) {
            for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
                if (f->type) register_generic_type(f->type);
            }
        }
    }
}

static void emit_mangled_type_name_ext(const AstTypeRef* type, FILE* out, bool erased) {
    if (!type || !type->parts) { fprintf(out, "int64_t"); return; }
    Str base = type->parts->text;
    
    // For mangling, we always use the Rae type name, not the C primitive name
    // to ensure we get valid C identifiers.
    fprintf(out, "rae_%.*s", (int)base.len, base.data);
    if (type->generic_args) {
        fprintf(out, "_");
        for (const AstTypeRef* a = type->generic_args; a; a = a->next) {
            if (erased) {
                fprintf(out, "Any_");
            } else if (a->generic_args) {
                emit_mangled_type_name_ext(a, out, erased);
            } else {
                Str ab = get_base_type_name(a);
                // If it looks like a generic parameter (single char), call it Any
                if (ab.len == 1 && ab.data[0] >= 'A' && ab.data[0] <= 'Z') {
                    fprintf(out, "Any_");
                } else {
                    fprintf(out, "%.*s_", (int)ab.len, ab.data);
                }
            }
        }
    }
}

static void emit_mangled_type_name(const AstTypeRef* type, FILE* out) {
    emit_mangled_type_name_ext(type, out, false);
}

static char* mangled_type_name_to_cstr(const AstTypeRef* type) {
    FILE* tmp = tmpfile();
    if (!tmp) return strdup("unknown");
    emit_mangled_type_name(type, tmp);
    long len = ftell(tmp);
    if (len <= 0) {
        // Fallback for systems where ftell doesn't work as expected on writes
        fseek(tmp, 0, SEEK_END);
        len = ftell(tmp);
    }
    rewind(tmp);
    char* result = malloc(len + 1);
    if (fread(result, 1, len, tmp) != (size_t)len) {
        free(result);
        fclose(tmp);
        return strdup("unknown");
    }
    result[len] = '\0';
    fclose(tmp);
    return result;
}

static bool emit_specialized_struct_def(const AstModule* module, const AstTypeRef* type, FILE* out, bool uses_raylib) {
    (void)uses_raylib;
    if (!type || !type->generic_args) return true;
    
    for (size_t i = 0; i < g_emitted_generic_type_count; i++) {
        if (type_refs_equal(g_emitted_generic_types[i], type)) return true;
    }
    if (g_emitted_generic_type_count < 512) {
        g_emitted_generic_types[g_emitted_generic_type_count++] = type;
    }

    Str base = type->parts->text;
    if (str_eq_cstr(base, "List")) {
        // Special case for native List(T)
        fprintf(out, "struct ");
        emit_mangled_type_name(type, out);
        fprintf(out, " {\n");
        fprintf(out, "  ");
        // Use a dummy context with the generic arg itself as a generic param
        // so that if it is a generic param, it gets mapped to RaeAny.
        CFuncContext dummy_ctx = {0};
        if (type->generic_args && type->generic_args->parts) {
            dummy_ctx.generic_params = type->generic_args->parts;
        }
        emit_type_ref_as_c_type(&dummy_ctx, type->generic_args, out);
        fprintf(out, "* data;\n");
        fprintf(out, "  int64_t length;\n");
        fprintf(out, "  int64_t capacity;\n");
        fprintf(out, "};\n\n");
        return true;
    }

    const AstDecl* d = find_type_decl(module, base);
    if (!d || d->kind != AST_DECL_TYPE) return true;

    fprintf(out, "struct ");
    emit_mangled_type_name(type, out);
    fprintf(out, " {\n");
    const AstTypeField* f = d->as.type_decl.fields;
    while (f) {
        bool substituted = false;
        if (f->type && f->type->parts && !f->type->generic_args) {
            const AstTypeRef* arg = type->generic_args;
            for (const AstIdentifierPart* gp = d->as.type_decl.generic_params; gp && arg; gp = gp->next, arg = arg->next) {
                if (str_eq(gp->text, f->type->parts->text)) {
                    fprintf(out, "  ");
                    emit_type_ref_as_c_type(NULL, arg, out);
                    fprintf(out, " %.*s;\n", (int)f->name.len, f->name.data);
                    substituted = true;
                    break;
                }
            }
        }
        if (!substituted) {
            fprintf(out, "  ");
            emit_type_ref_as_c_type(NULL, f->type, out);
            fprintf(out, " %.*s;\n", (int)f->name.len, f->name.data);
        }
        f = f->next;
    }
    fprintf(out, "};\n\n");
    return true;
}

static int binary_op_precedence(AstBinaryOp op) {
  switch (op) {
    case AST_BIN_ADD: return PREC_ADD;
    case AST_BIN_SUB: return PREC_ADD;
    case AST_BIN_MUL: return PREC_MUL;
    case AST_BIN_DIV: return PREC_MUL;
    case AST_BIN_MOD: return PREC_MUL;
    case AST_BIN_LT: return PREC_RELATIONAL;
    case AST_BIN_GT: return PREC_RELATIONAL;
    case AST_BIN_LE: return PREC_RELATIONAL;
    case AST_BIN_GE: return PREC_RELATIONAL;
    case AST_BIN_IS: return PREC_EQUALITY;
    case AST_BIN_AND: return PREC_LOGICAL_AND;
    case AST_BIN_OR: return PREC_LOGICAL_OR;
  }
  return PREC_LOWEST;
}

// Helper functions for emitting literals
static bool emit_string_literal(FILE* out, Str literal) {
  if (fprintf(out, "\"") < 0) return false;
  for (size_t i = 0; i < literal.len; i++) {
    char c = literal.data[i];
    switch (c) {
      case '"': fprintf(out, "\\\""); break;
      case '\\': fprintf(out, "\\\\"); break;
      case '\n': fprintf(out, "\\n"); break;
      case '\r': fprintf(out, "\\r"); break;
      case '\t': fprintf(out, "\\t"); break;
      default: fputc(c, out); break;
    }
  }
  return fprintf(out, "\"") >= 0;
}

static const char* map_rae_type_to_c(Str type_name) {
  if (str_eq_cstr(type_name, "Int")) return "int64_t";
  if (str_eq_cstr(type_name, "Float")) return "double";
  if (str_eq_cstr(type_name, "Bool")) return "int8_t";
  if (str_eq_cstr(type_name, "Char")) return "int64_t";
  if (str_eq_cstr(type_name, "String")) return "const char*";
  if (str_eq_cstr(type_name, "Buffer")) return "void*";
  if (str_eq_cstr(type_name, "Any")) return "RaeAny";
  if (str_eq_cstr(type_name, "Texture")) return "Texture";
  if (str_eq_cstr(type_name, "Color")) return "Color";
  if (str_eq_cstr(type_name, "Vector2")) return "Vector2";
  if (str_eq_cstr(type_name, "Vector3")) return "Vector3";
  if (str_eq_cstr(type_name, "Camera3D")) return "Camera3D";
  return NULL;
}

static bool emit_type_ref_as_c_type(CFuncContext* ctx, const AstTypeRef* type, FILE* out) {
  if (!type || !type->parts) return fprintf(out, "int64_t") >= 0; // Default to int64_t for unknown types

  const AstIdentifierPart* current = type->parts;
  bool is_ptr = type->is_view || type->is_mod;

  if (str_eq_cstr(current->text, "Buffer")) {
      if (type->generic_args && type->generic_args->parts) {
          Str ab = get_base_type_name(type->generic_args);
          const char* inner = map_rae_type_to_c(ab);
          if (inner) {
              fprintf(out, "%s*", inner);
          } else {
              // Check if it's a generic param or needs mangling
              bool is_generic = false;
              if (ctx && ctx->generic_params) {
                  const AstIdentifierPart* gp = ctx->generic_params;
                  while (gp) {
                      if (str_eq(gp->text, ab)) { is_generic = true; break; }
                      gp = gp->next;
                  }
              }
              if (is_generic || (ab.len == 1 && ab.data[0] >= 'A' && ab.data[0] <= 'Z')) {
                  fprintf(out, "RaeAny*");
              } else {
                  emit_mangled_type_name(type->generic_args, out);
                  fprintf(out, "*");
              }
          }
          return true;
      }
      return fprintf(out, "void*") >= 0;
  }

  if (type->generic_args) {
    emit_mangled_type_name(type, out);
    if (is_ptr) fprintf(out, "*");
    return true;
  }

  if (type->is_id) {
      return fprintf(out, "int64_t") >= 0;
  }
  if (type->is_key) {
      return fprintf(out, "const char*") >= 0;
  }
  
  if (type->is_opt) {
      return fprintf(out, "RaeAny") >= 0;
  }

  if (str_eq_cstr(current->text, "Buffer")) {
      if (type->generic_args && type->generic_args->parts) {
          const char* inner = map_rae_type_to_c(type->generic_args->parts->text);
          if (inner) {
              fprintf(out, "%s*", inner);
          } else {
              // Check if it's a generic param
              bool is_generic = false;
              if (ctx && ctx->generic_params) {
                  const AstIdentifierPart* gp = ctx->generic_params;
                  while (gp) {
                      if (str_eq(gp->text, type->generic_args->parts->text)) {
                          is_generic = true;
                          break;
                      }
                      gp = gp->next;
                  }
              }
              if (is_generic) {
                  fprintf(out, "RaeAny*");
              } else {
                  fprintf(out, "%.*s*", (int)type->generic_args->parts->text.len, type->generic_args->parts->text.data);
              }
          }
      } else {
          fprintf(out, "void*");
      }
      return true;
  }

  const char* c_type_str = map_rae_type_to_c(current->text);
  if (c_type_str) {
      if (fprintf(out, "%s", c_type_str) < 0) return false;
  } else {
      // Check if it's a generic param
      bool is_generic = false;
      if (ctx && ctx->generic_params) {
          const AstIdentifierPart* gp = ctx->generic_params;
          while (gp) {
              if (str_eq(gp->text, current->text)) {
                  is_generic = true;
                  break;
              }
              gp = gp->next;
          }
      }

      if (is_generic) {
          if (fprintf(out, "RaeAny") < 0) return false;
          is_ptr = false; // RaeAny is a value type
      } else if (current->text.len == 1 && current->text.data[0] >= 'A' && current->text.data[0] <= 'Z') {
          if (fprintf(out, "RaeAny") < 0) return false;
          is_ptr = false;
      } else {
          if (fprintf(out, "%.*s", (int)current->text.len, current->text.data) < 0) return false;
          
          while (current->next) { // Handle multi-part identifiers (e.g., Module.Type)
              current = current->next;
              if (fprintf(out, "_%.*s", (int)current->text.len, current->text.data) < 0) return false; // Concatenate with underscore
          }
      }
  }

  // Update is_ptr for non-explicit view/mod if it's not a primitive and not explicitly val
  if (!is_ptr && !type->is_val && !is_primitive_type(current->text)) {
      // For local variables (not params), they are owned values by default.
      // But wait, this function is used for let statements TOO.
      // If it's a let statement, we usually want the owned type.
      // is_ptr is true ONLY if explicitly view/mod.
  }

  if (is_ptr && !type->is_val) {
      if (fprintf(out, "*") < 0) return false;
  }

  if (type->generic_args) {
      // C backend doesn't currently support generic types directly in type names
      // For now, we'll just ignore them or emit a warning if needed
      // fprintf(stderr, "warning: C backend does not yet fully support generic type arguments\n");
  }

  return true;
}

static bool emit_param_list(CFuncContext* ctx, const AstParam* params, FILE* out, bool is_extern) {
  size_t index = 0;
  const AstParam* param = params;
  if (!param) {
    return fprintf(out, "void") >= 0;
  }
  while (param) {
    if (index > 0) {
      if (fprintf(out, ", ") < 0) return false;
    }
    
    const char* c_type_base = NULL;
    bool is_ptr = false;
    char* free_me = NULL;

    if (param->type) {
        if (param->type->is_id) {
            c_type_base = "int64_t";
        } else if (param->type->is_key) {
            c_type_base = "const char*";
        } else if (param->type->parts) {
            Str first = param->type->parts->text;
            
            // SEMANTICS: view-by-default
            // mod T -> T*
            // val T -> T
            // T     -> const T* (semantically view)
            // Exception: primitives are passed by value even if 'view'
            
            bool is_mod = param->type->is_mod;
            bool is_val = param->type->is_val;
            bool is_explicit_view = param->type->is_view;
            bool is_primitive = is_primitive_type(first) || (ctx && ctx->uses_raylib && is_raylib_builtin_type(first));
            
            // Treat enums as primitives (value types)
            if (!is_primitive && ctx && ctx->module) {
                if (find_enum_decl(ctx->module, first)) {
                    is_primitive = true;
                }
            }
            
            if (is_extern) {
                is_ptr = (is_mod || is_explicit_view);
            } else {
                is_ptr = (is_mod || is_explicit_view || (!is_val && !is_primitive));
            }
            bool is_const = (!is_mod && is_ptr);

            c_type_base = map_rae_type_to_c(first);
            if (!c_type_base) {
                // Check if it's a generic param
                bool is_generic = false;
                if (ctx && ctx->generic_params) {
                    const AstIdentifierPart* gp = ctx->generic_params;
                    while (gp) {
                        if (str_eq(gp->text, first)) {
                            is_generic = true;
                            break;
                        }
                        gp = gp->next;
                    }
                }
                if (is_generic) {
                    c_type_base = "RaeAny";
                    is_ptr = false; // RaeAny is already a value type (container)
                    is_const = false;
                } else if (param->type->generic_args) {
                    c_type_base = free_me = mangled_type_name_to_cstr(param->type);
                } else {
                    c_type_base = free_me = str_to_cstr(first);
                }
            }

            bool is_already_const_param = (c_type_base && strstr(c_type_base, "const ") == c_type_base);

            if (is_const && !is_already_const_param) {
                char* buf = malloc(strlen(c_type_base) + 16);
                sprintf(buf, "const %s", c_type_base);
                if (free_me) free(free_me);
                c_type_base = free_me = buf;
            }
        }
    }
    if (!c_type_base) c_type_base = "int64_t";

    if (fprintf(out, "%s%s %.*s", c_type_base, is_ptr ? "*" : "", (int)param->name.len, param->name.data) < 0) {
      if (free_me) free(free_me);
      return false;
    }
    if (free_me) free(free_me);
    param = param->next;
    index += 1;
  }
  return true;
}

static bool func_has_return_value(const AstFuncDecl* func) {
  if (func->returns) return true;
  if (!func->body) return false;
  const AstStmt* stmt = func->body->first;
  while (stmt) {
    if (stmt->kind == AST_STMT_RET && stmt->as.ret_stmt.values) return true;
    stmt = stmt->next;
  }
  return false;
}

static const char* c_return_type(CFuncContext* ctx, const AstFuncDecl* func) {
  if (func->returns) {
    if (func->returns->next) {
      fprintf(stderr, "error: C backend only supports single return values per function\n");
      return NULL;
    }
    bool is_view = func->returns->type->is_view;
    bool is_ptr = is_view || func->returns->type->is_mod;
    
    if (func->returns->type->is_id) return "int64_t";
    if (func->returns->type->is_key) return "const char*";

    // Check if it's a generic param
    bool is_generic = false;
    const AstIdentifierPart* gps[] = { ctx ? ctx->generic_params : NULL, func->generic_params };
    for (int i = 0; i < 2; i++) {
        const AstIdentifierPart* gp = gps[i];
        while (gp) {
            if (str_eq(gp->text, func->returns->type->parts->text)) {
                is_generic = true;
                break;
            }
            gp = gp->next;
        }
        if (is_generic) break;
    }
    
    if (is_generic) {
        return "RaeAny";
    }

    // Check if it's a generic type specialization
    if (func->returns->type->generic_args) {
        char* mangled = mangled_type_name_to_cstr(func->returns->type);
        if (is_ptr) {
            char* buf = malloc(strlen(mangled) + 16);
            sprintf(buf, "%s%s*", (is_view) ? "const " : "", mangled);
            free(mangled);
            return buf; // leak
        }
        return mangled; // leak
    }

    const char* mapped = map_rae_type_to_c(func->returns->type->parts->text);
    bool is_already_const = (mapped && strcmp(mapped, "const char*") == 0);
    if (mapped) {
        if (is_ptr) {
            char* buf = malloc(strlen(mapped) + 16);
            sprintf(buf, "%s%s*", (is_view && !is_already_const) ? "const " : "", mapped);
            return buf; // leak
        }
        return mapped;
    }

    char* type_name = str_to_cstr(func->returns->type->parts->text);
    bool is_already_const_custom = (strcmp(type_name, "const char*") == 0);
    if (is_ptr) {
        char* buf = malloc(strlen(type_name) + 16);
        sprintf(buf, "%s%s*", (is_view && !is_already_const_custom) ? "const " : "", type_name);
        free(type_name);
        return buf; // leak
    }
    return type_name; // leak
  }
  
  if (func_has_return_value(func)) {
      return "int64_t";
  }
  
  return "void";
}

static Str get_local_type_name(CFuncContext* ctx, Str name);
static Str infer_expr_type(CFuncContext* ctx, const AstExpr* expr);

static const AstTypeRef* infer_buffer_element_type_ref(CFuncContext* ctx, const AstExpr* expr) {
    if (expr->kind == AST_EXPR_IDENT) {
        const AstTypeRef* type = get_local_type_ref(ctx, expr->as.ident);
        if (type && type->parts && str_eq_cstr(type->parts->text, "Buffer")) {
            return type->generic_args;
        }
    } else if (expr->kind == AST_EXPR_MEMBER) {
        Str obj_type = infer_expr_type(ctx, expr->as.member.object);
        if (obj_type.len > 0) {
            const AstDecl* d = find_type_decl(ctx->module, obj_type);
            if (d && d->kind == AST_DECL_TYPE) {
                for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
                    if (str_eq(f->name, expr->as.member.member)) {
                        if (f->type && f->type->parts && str_eq_cstr(f->type->parts->text, "Buffer")) {
                            return f->type->generic_args;
                        }
                    }
                }
            }
        }
    }
    return NULL;
}

static const AstExpr* g_infer_expr_stack[64];
static size_t g_infer_expr_stack_count = 0;

static Str infer_expr_type(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr) return (Str){0};
    
    for (size_t i = 0; i < g_infer_expr_stack_count; i++) {
        if (g_infer_expr_stack[i] == expr) return (Str){0};
    }
    if (g_infer_expr_stack_count >= 64) return (Str){0};
    g_infer_expr_stack[g_infer_expr_stack_count++] = expr;

    Str res = {0};
    switch (expr->kind) {
        case AST_EXPR_IDENT:
            res = get_local_type_name(ctx, expr->as.ident);
            break;
        case AST_EXPR_INTEGER: res = str_from_cstr("Int"); break;
        case AST_EXPR_FLOAT: res = str_from_cstr("Float"); break;
        case AST_EXPR_BOOL: res = str_from_cstr("Bool"); break;
        case AST_EXPR_STRING: res = str_from_cstr("String"); break;
        case AST_EXPR_CHAR: res = str_from_cstr("Char"); break;
        case AST_EXPR_CALL: {
            if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
                Str name = expr->as.call.callee->as.ident;
                
                if (str_eq_cstr(name, "rae_str") || str_eq_cstr(name, "rae_str_concat") || str_eq_cstr(name, "rae_str_sub") || str_eq_cstr(name, "rae_sys_get_env") || str_eq_cstr(name, "rae_io_read_line") || str_eq_cstr(name, "rae_io_read_char") || str_eq_cstr(name, "rae_sys_read_file")) {
                    res = str_from_cstr("String");
                    goto done;
                }
                
                uint16_t arg_count = 0;
                for (const AstCallArg* arg = expr->as.call.args; arg; arg = arg->next) arg_count++;
                
                Str* arg_types = NULL;
                if (arg_count > 0) {
                    arg_types = malloc(arg_count * sizeof(Str));
                    const AstCallArg* arg = expr->as.call.args;
                    for (uint16_t i = 0; i < arg_count; ++i) {
                        arg_types[i] = infer_expr_type(ctx, arg->value);
                        arg = arg->next;
                    }
                }
                
                const AstFuncDecl* d = find_function_overload(ctx->module, ctx, name, arg_types, arg_count);
                free(arg_types);
                
                if (d) {
                    if (d->returns && d->returns->type && d->returns->type->parts) {
                        Str rtype = d->returns->type->parts->text;
                        
                        // Check if rtype is a generic parameter of the function
                        const AstIdentifierPart* gp = d->generic_params;
                        while (gp) {
                            if (str_eq(gp->text, rtype)) { 
                                // It's generic return. Try to specialize if it's a known generic container method
                                if (expr->as.call.args) {
                                    const AstExpr* first_arg = expr->as.call.args->value;
                                    const AstExpr* inner_obj = first_arg;
                                    while (inner_obj->kind == AST_EXPR_UNARY && (inner_obj->as.unary.op == AST_UNARY_VIEW || inner_obj->as.unary.op == AST_UNARY_MOD)) {
                                        inner_obj = inner_obj->as.unary.operand;
                                    }
                                    if (inner_obj->kind == AST_EXPR_IDENT) {
                                        const AstTypeRef* tr = get_local_type_ref(ctx, inner_obj->as.ident);
                                        if (tr && tr->generic_args && tr->generic_args->parts) {
                                            res = tr->generic_args->parts->text;
                                            goto done;
                                        }
                                    }
                                }
                                res = str_from_cstr("Any"); 
                                goto done; 
                            }
                            gp = gp->next;
                        }
                        
                        res = rtype;
                        goto done;
                    }
                }

                // HACK: for positional 'get' sugar (fallback)
                if (str_eq_cstr(name, "get") && expr->as.call.args) {
                    const AstExpr* obj = expr->as.call.args->value;
                    const AstExpr* inner = obj;
                    while (inner->kind == AST_EXPR_UNARY && (inner->as.unary.op == AST_UNARY_VIEW || inner->as.unary.op == AST_UNARY_MOD)) {
                        inner = inner->as.unary.operand;
                    }
                    
                    if (inner->kind == AST_EXPR_IDENT) {
                        const AstTypeRef* type = get_local_type_ref(ctx, inner->as.ident);
                        if (type && type->parts && (str_eq_cstr(type->parts->text, "List") || str_eq_cstr(type->parts->text, "List2"))) {
                            if (type->generic_args && type->generic_args->parts) {
                                res = type->generic_args->parts->text;
                                goto done;
                            }
                        }
                    }
                    Str obj_type = infer_expr_type(ctx, obj);
                    if (str_eq_cstr(obj_type, "List2") || str_eq_cstr(obj_type, "Any")) { res = str_from_cstr("Any"); goto done; }
                    if (str_eq_cstr(obj_type, "List2Int")) { res = str_from_cstr("Int"); goto done; }
                }
            }
            break;
        }
        case AST_EXPR_MEMBER: {
            if (expr->as.member.object->kind == AST_EXPR_IDENT) {
                // Check if it's an enum (e.g. EntityKind.Cube)
                if (find_enum_decl(ctx->module, expr->as.member.object->as.ident)) {
                    res = expr->as.member.object->as.ident;
                    goto done;
                }
            }
            Str obj_type = infer_expr_type(ctx, expr->as.member.object);
            if (obj_type.len > 0) {
                const AstDecl* d = find_type_decl(ctx->module, obj_type);
                if (d && d->kind == AST_DECL_TYPE) {
                    for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
                        if (str_eq(f->name, expr->as.member.member)) {
                                                                                                                            if (f->type && f->type->parts) {
                                                                                                                                Str ftype = f->type->parts->text;
                                                                                                                                if (str_eq_cstr(ftype, "Buffer") && f->type->generic_args && f->type->generic_args->parts) {
                                                                                                                                    ftype = f->type->generic_args->parts->text;
                                                                                                                                }
                                                                                                                                
                                                                                                                                // Check if ftype is a generic parameter of the type d
                                                                                                                                const AstIdentifierPart* gp = d->as.type_decl.generic_params;
                                                                                                                                while (gp) {
                                                                                                                                    if (str_eq(gp->text, ftype)) { res = str_from_cstr("Any"); goto done; }
                                                                                                                                    gp = gp->next;
                                                                                                                                }
                                                                                                                                res = ftype;
                                                                                                                                goto done;
                                                                                                                            }
                        }
                    }
                }
            }
            break;
        }
        case AST_EXPR_METHOD_CALL: {
            const AstExpr* obj = expr->as.method_call.object;
            const AstExpr* inner = obj;
            while (inner->kind == AST_EXPR_UNARY && (inner->as.unary.op == AST_UNARY_VIEW || inner->as.unary.op == AST_UNARY_MOD)) {
                inner = inner->as.unary.operand;
            }
            
            if (inner->kind == AST_EXPR_IDENT) {
                const AstTypeRef* type = get_local_type_ref(ctx, inner->as.ident);
                if (type && type->generic_args && type->generic_args->parts) {
                    res = type->generic_args->parts->text;
                    goto done;
                }
            } else if (inner->kind == AST_EXPR_MEMBER) {
                // Try to infer type of member
                Str obj_type = infer_expr_type(ctx, inner->as.member.object);
                const AstDecl* d = find_type_decl(ctx->module, obj_type);
                if (d && d->kind == AST_DECL_TYPE) {
                    for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
                        if (str_eq(f->name, inner->as.member.member)) {
                            if (f->type && f->type->generic_args && f->type->generic_args->parts) {
                                res = f->type->generic_args->parts->text;
                                goto done;
                            }
                        }
                    }
                }
            }
            Str obj_type = infer_expr_type(ctx, obj);
            if (str_eq_cstr(obj_type, "List2Int")) { res = str_from_cstr("Int"); goto done; }
            
            if (str_eq_cstr(expr->as.method_call.method_name, "get")) {
                if (str_eq_cstr(obj_type, "List") || str_eq_cstr(obj_type, "List2") || str_eq_cstr(obj_type, "Any")) { res = str_from_cstr("Any"); goto done; }
            }
            break;
        }
        case AST_EXPR_UNARY: {
            if (expr->as.unary.op == AST_UNARY_NOT) { res = str_from_cstr("Bool"); goto done; }
            Str inner = infer_expr_type(ctx, expr->as.unary.operand);
            if (str_eq_cstr(inner, "Float")) { res = str_from_cstr("Float"); goto done; }
            res = inner;
            break;
        }
        case AST_EXPR_BINARY: {
            switch (expr->as.binary.op) {
                case AST_BIN_LT: case AST_BIN_GT: case AST_BIN_LE: case AST_BIN_GE:
                case AST_BIN_IS: case AST_BIN_AND: case AST_BIN_OR:
                    res = str_from_cstr("Bool");
                    goto done;
                default: break;
            }
            Str lhs = infer_expr_type(ctx, expr->as.binary.lhs);
            Str rhs = infer_expr_type(ctx, expr->as.binary.rhs);
            if (str_eq_cstr(lhs, "Float") || str_eq_cstr(rhs, "Float")) { res = str_from_cstr("Float"); goto done; }
            res = lhs;
            break;
        }
        case AST_EXPR_INTERP:
            res = str_from_cstr("String");
            break;
        case AST_EXPR_INDEX:
            // Simplified: default to Any for now
            res = str_from_cstr("Any");
            break;
        case AST_EXPR_OBJECT:
            if (expr->as.object_literal.type && expr->as.object_literal.type->parts) {
                res = expr->as.object_literal.type->parts->text;
            } else if (ctx->expected_type && ctx->expected_type->parts) {
                res = ctx->expected_type->parts->text;
            }
            break;
        default: break;
    }
done:
    g_infer_expr_stack_count--;
    return res;
}

static bool is_string_expr(const AstExpr* expr) {
  if (!expr) return false;
  if (expr->kind == AST_EXPR_STRING || expr->kind == AST_EXPR_INTERP) return true;
  if (expr->kind == AST_EXPR_MEMBER) {
      if (str_eq_cstr(expr->as.member.member, "name")) return true;
  }
  if (expr->kind == AST_EXPR_CALL && expr->as.call.callee->kind == AST_EXPR_IDENT) {
    Str name = expr->as.call.callee->as.ident;
    if (str_eq_cstr(name, "rae_str") || str_eq_cstr(name, "rae_str_concat")) return true;
    
    // Recursive check for nested concats
    const AstCallArg* arg = expr->as.call.args;
    while (arg) {
        if (is_string_expr(arg->value)) return true;
        arg = arg->next;
    }
  }
  return false;
}

static bool emit_log_call(CFuncContext* ctx, const AstExpr* expr, FILE* out, bool newline) {
  const AstCallArg* arg = expr->as.call.args;
  if (!arg || arg->next) {
    fprintf(stderr,
            "error: C backend expects exactly one argument for log/logS during codegen\n");
    return false;
  }
  const AstExpr* value = arg->value;
  
  Str type_name = infer_expr_type(ctx, value);
  const char* log_fn = newline ? "rae_ext_rae_log_any" : "rae_ext_rae_log_stream_any";
  bool is_list = false;

  if (str_eq_cstr(type_name, "Int")) {
      log_fn = newline ? "rae_ext_rae_log_i64" : "rae_ext_rae_log_stream_i64";
  } else if (str_eq_cstr(type_name, "Float")) {
      log_fn = newline ? "rae_ext_rae_log_float" : "rae_ext_rae_log_stream_float";
  } else if (str_eq_cstr(type_name, "Bool")) {
      log_fn = newline ? "rae_ext_rae_log_bool" : "rae_ext_rae_log_stream_bool";
  } else if (str_eq_cstr(type_name, "Char")) {
      log_fn = newline ? "rae_ext_rae_log_char" : "rae_ext_rae_log_stream_char";
  } else if (str_eq_cstr(type_name, "String")) {
      log_fn = newline ? "rae_ext_rae_log_cstr" : "rae_ext_rae_log_stream_cstr";
  } else if (str_eq_cstr(type_name, "id")) {
      log_fn = newline ? "rae_ext_rae_log_id" : "rae_ext_rae_log_stream_id";
  } else if (str_eq_cstr(type_name, "key")) {
      log_fn = newline ? "rae_ext_rae_log_key" : "rae_ext_rae_log_stream_key";
  } else if (str_eq_cstr(type_name, "List")) {
      is_list = true;
  } else if (find_enum_decl(ctx->module, type_name)) {
      log_fn = newline ? "rae_ext_rae_log_i64" : "rae_ext_rae_log_stream_i64";
  }

  bool val_is_ptr = (value->kind == AST_EXPR_IDENT && is_pointer_type(ctx, value->as.ident));
  if (val_is_ptr) {
      fprintf(out, "  rae_ext_rae_log_stream_cstr(\"%s \");\n", is_mod_type(ctx, value->as.ident) ? "mod" : "view");
  }

  if (is_list) {
    if (fprintf(out, "  %s((RaeAny*)", newline ? "rae_ext_rae_log_list_fields" : "rae_ext_rae_log_stream_list_fields") < 0) return false;
    if (!emit_expr(ctx, value, out, PREC_LOWEST, false)) return false;
    fprintf(out, ".data, ");
    if (!emit_expr(ctx, value, out, PREC_LOWEST, false)) return false;
    fprintf(out, ".length, ");
    if (!emit_expr(ctx, value, out, PREC_LOWEST, false)) return false;
    fprintf(out, ".capacity);\n");
    return true;
  }

  bool is_generic = strcmp(log_fn, "rae_ext_rae_log_any") == 0 || strcmp(log_fn, "rae_ext_rae_log_stream_any") == 0;
  bool is_enum = find_enum_decl(ctx->module, type_name) != NULL;

  if (fprintf(out, "  %s(", log_fn) < 0) return false;
  if (is_generic) fprintf(out, "rae_any(");
  if (is_enum) fprintf(out, "(int64_t)(");
  if (val_is_ptr) fprintf(out, "(*");
  if (!emit_expr(ctx, value, out, PREC_LOWEST, false)) return false;
  if (val_is_ptr) fprintf(out, ")");
  if (is_enum) fprintf(out, ")");
  if (is_generic) fprintf(out, ")");
  if (fprintf(out, ");\n") < 0) return false;
  
  return true;
}

static bool is_primitive_type(Str type_name) {
    return str_eq_cstr(type_name, "Int") || 
           str_eq_cstr(type_name, "Float") || 
           str_eq_cstr(type_name, "Bool") || 
           str_eq_cstr(type_name, "Char") ||
           str_eq_cstr(type_name, "String") ||
           str_eq_cstr(type_name, "Array") ||
           str_eq_cstr(type_name, "Buffer") ||
           str_eq_cstr(type_name, "Any");
}

static bool is_raylib_builtin_type(Str type_name) {
    return str_eq_cstr(type_name, "Vector2") || 
           str_eq_cstr(type_name, "Vector3") || 
           str_eq_cstr(type_name, "Color") ||
           str_eq_cstr(type_name, "Texture") ||
           str_eq_cstr(type_name, "Camera3D");
}
  
         static Str strip_generics(Str type_name) {
  for (size_t i = 0; i < type_name.len; ++i) {
    if (type_name.data[i] == '(') {
      return (Str){.data = type_name.data, .len = i};
    }
  }
  return type_name;
}

static Str get_base_type_name_str(Str type_name) {
  Str res = type_name;
  if (str_starts_with_cstr(res, "mod ")) {
    res.data += 4;
    res.len -= 4;
  } else if (str_starts_with_cstr(res, "view ")) {
    res.data += 5;
    res.len -= 5;
  } else if (str_starts_with_cstr(res, "opt ")) {
    res.data += 4;
    res.len -= 4;
  }
  return res;
}

static bool types_match(CFuncContext* ctx, Str entry_type_raw, Str call_type_raw) {
  Str entry_type = get_base_type_name_str(entry_type_raw);
  Str call_type = get_base_type_name_str(call_type_raw);

  // Strip generics for base comparison
  Str entry_base_no_gen = strip_generics(entry_type);
  Str call_base_no_gen = strip_generics(call_type);

  // 1. Exact match
  if (str_eq(entry_type, call_type)) return true;
  if (str_eq(entry_base_no_gen, call_base_no_gen) && entry_type.len == entry_base_no_gen.len && call_type.len == call_base_no_gen.len) return true;
  
  // 2. Generic placeholder (single uppercase letter like 'T')
  // If entry type is a placeholder, it matches anything
  if (entry_type.len == 1 && entry_type.data[0] >= 'A' && entry_type.data[0] <= 'Z') {
    return true;
  }
  
  // Also check if entry_type is a generic parameter of the CURRENT function being compiled
  if (ctx && ctx->generic_params) {
      const AstIdentifierPart* gp = ctx->generic_params;
      while (gp) {
          if (str_eq(gp->text, entry_type)) return true;
          gp = gp->next;
      }
  }
  
  // Call 'Any' matches any entry type
  if (str_eq_cstr(call_type, "Any")) {
      return true;
  }
  
  // Entry 'Any' matches any call type
  if (str_eq_cstr(entry_type, "Any")) {
      return true;
  }

  // 3. Complex generic match: List(T) vs List(Int)
  Str entry_base = strip_generics(entry_type);
  Str call_base = strip_generics(call_type);
  
  if (str_eq(entry_base, call_base)) {
    if (entry_type.len > entry_base.len && call_type.len > call_base.len) {
        // Both have generics, match the inner part
        Str entry_inner = { .data = entry_type.data + entry_base.len + 1, .len = entry_type.len - entry_base.len - 2 };
        Str call_inner = { .data = call_type.data + call_base.len + 1, .len = call_type.len - call_base.len - 2 };
        return types_match(ctx, entry_inner, call_inner);
    }
    // If entry has generics but call doesn't, it's a match (e.g. List(T) matches List)
    if (entry_type.len > entry_base.len && call_type.len == call_base.len) {
        return true;
    }
  }
  
  return false;
}

static const AstFuncDecl* find_function_overload(const AstModule* module, CFuncContext* ctx, Str name, const Str* param_types, uint16_t param_count) {
    if (!module) return NULL;
    
    // Phase 1: Exact name matches
    for (const AstDecl* d = module->decls; d; d = d->next) {
        if (d->kind == AST_DECL_FUNC) {
            const AstFuncDecl* fd = &d->as.func_decl;
            uint16_t fd_param_count = 0;
            for (const AstParam* p = fd->params; p; p = p->next) fd_param_count++;

            if (str_eq(fd->name, name)) {
                if (fd_param_count == param_count) {
                    if (param_types) {
                        bool mismatch = false;
                        const AstParam* p = fd->params;
                        for (uint16_t j = 0; j < param_count; ++j) {
                            if (param_types[j].len == 0) continue;
                            Str fd_type = d_get_param_type_name(p);
                            // We MUST use types_match here, not str_eq, because fd_type might be a generic parameter!
                            if (!types_match(ctx, fd_type, param_types[j])) {
                                mismatch = true;
                                break;
                            }
                            p = p->next;
                        }
                        if (!mismatch) return fd;
                    } else {
                        return fd;
                    }
                }
            }
        }
    }

    // Phase 2: Search in imported modules
    for (const AstImport* imp = module->imports; imp; imp = imp->next) {
        if (!imp->module) continue;
        const AstFuncDecl* found = find_function_overload(imp->module, ctx, name, param_types, param_count);
        if (found) return found;
    }

    // Phase 3: Mangled generic matches
    // Only used when 'name' is NOT a simple name but a mangled one
    // AND it starts with 'rae_'
    char* n_cstr = str_to_cstr(name);
    if (strncmp(n_cstr, "rae_", 4) == 0) {
        for (const AstDecl* d = module->decls; d; d = d->next) {
            if (d->kind == AST_DECL_FUNC) {
                const AstFuncDecl* fd = &d->as.func_decl;
                if (!fd->generic_params) continue;
                
                char* fdn_cstr = str_to_cstr(fd->name);
                char mangled_prefix[128];
                snprintf(mangled_prefix, sizeof(mangled_prefix), "rae_%s_", fdn_cstr);
                
                if (strncmp(n_cstr, mangled_prefix, strlen(mangled_prefix)) == 0) {
                    uint16_t mangled_param_count = 0;
                    for (const AstParam* p = fd->params; p; p = p->next) mangled_param_count++;
                    if (mangled_param_count == param_count) {
                        free(fdn_cstr);
                        free(n_cstr);
                        return fd;
                    }
                }
                free(fdn_cstr);
            }
        }
    }
    free(n_cstr);
    return NULL;
}

static void emit_mangled_function_name(const AstFuncDecl* func, FILE* out) {
    if (!func) { fprintf(out, "unknown"); return; }
    const char* ray_mapping = find_raylib_mapping(func->name);
    if (ray_mapping) {
        fprintf(out, "rae_ext_%.*s", (int)func->name.len, func->name.data);
        return;
    }

    if (func->is_extern) {
        Str name = func->name;
        if (str_eq_cstr(name, "sleep") || str_eq_cstr(name, "sleepMs")) {
            fprintf(out, "rae_ext_rae_sleep");
        } else if (str_eq_cstr(name, "rae_str") || str_eq_cstr(name, "str")) {
            fprintf(out, "rae_ext_rae_str");
        } else if (str_eq_cstr(name, "rae_str_len") || str_eq_cstr(name, "str_len")) {
            fprintf(out, "rae_ext_rae_str_len");
        } else if (str_eq_cstr(name, "rae_str_concat") || str_eq_cstr(name, "str_concat")) {
            fprintf(out, "rae_ext_rae_str_concat");
        } else if (str_eq_cstr(name, "rae_str_compare") || str_eq_cstr(name, "str_compare")) {
            fprintf(out, "rae_ext_rae_str_compare");
        } else if (str_eq_cstr(name, "rae_str_sub") || str_eq_cstr(name, "str_sub")) {
            fprintf(out, "rae_ext_rae_str_sub");
        } else if (str_eq_cstr(name, "rae_str_contains") || str_eq_cstr(name, "str_contains")) {
            fprintf(out, "rae_ext_rae_str_contains");
        } else if (str_eq_cstr(name, "rae_str_starts_with") || str_eq_cstr(name, "str_starts_with")) {
            fprintf(out, "rae_ext_rae_str_starts_with");
        } else if (str_eq_cstr(name, "rae_str_ends_with") || str_eq_cstr(name, "str_ends_with")) {
            fprintf(out, "rae_ext_rae_str_ends_with");
        } else if (str_eq_cstr(name, "rae_str_index_of") || str_eq_cstr(name, "str_index_of")) {
            fprintf(out, "rae_ext_rae_str_index_of");
        } else if (str_eq_cstr(name, "rae_str_trim") || str_eq_cstr(name, "str_trim")) {
            fprintf(out, "rae_ext_rae_str_trim");
        } else if (str_eq_cstr(name, "rae_str_to_f64") || str_eq_cstr(name, "str_to_float")) {
            fprintf(out, "rae_ext_rae_str_to_f64");
        } else if (str_eq_cstr(name, "rae_str_to_i64") || str_eq_cstr(name, "str_to_int")) {
            fprintf(out, "rae_ext_rae_str_to_i64");
        } else if (str_eq_cstr(name, "getEnv")) {
            fprintf(out, "rae_ext_rae_sys_get_env");
        } else if (str_eq_cstr(name, "exit")) {
            fprintf(out, "rae_ext_rae_sys_exit");
        } else if (str_eq_cstr(name, "readFile")) {
            fprintf(out, "rae_ext_rae_sys_read_file");
        } else if (str_eq_cstr(name, "writeFile")) {
            fprintf(out, "rae_ext_rae_sys_write_file");
        } else if (str_eq_cstr(name, "nextTick")) {
            fprintf(out, "rae_ext_nextTick");
        } else if (str_eq_cstr(name, "nowMs")) {
            fprintf(out, "rae_ext_nowMs");
        } else if (str_eq_cstr(name, "rae_random") || str_eq_cstr(name, "random")) {
            fprintf(out, "rae_ext_rae_random");
        } else if (str_eq_cstr(name, "rae_seed") || str_eq_cstr(name, "seed")) {
            fprintf(out, "rae_ext_rae_seed");
        } else if (str_eq_cstr(name, "rae_random_int") || str_eq_cstr(name, "random_int")) {
            fprintf(out, "rae_ext_rae_random_int");
        } else if (str_eq_cstr(name, "rae_int_to_float")) {
            fprintf(out, "rae_ext_rae_int_to_float");
        } else if (str_eq_cstr(name, "readLine")) {
            fprintf(out, "rae_ext_rae_io_read_line");
        } else if (str_eq_cstr(name, "readChar")) {
            fprintf(out, "rae_ext_rae_io_read_char");
        } else if (str_eq_cstr(name, "sin")) {
            fprintf(out, "rae_ext_rae_math_sin");
        } else if (str_eq_cstr(name, "cos")) {
            fprintf(out, "rae_ext_rae_math_cos");
        } else if (str_eq_cstr(name, "tan")) {
            fprintf(out, "rae_ext_rae_math_tan");
        } else if (str_eq_cstr(name, "asin")) {
            fprintf(out, "rae_ext_rae_math_asin");
        } else if (str_eq_cstr(name, "acos")) {
            fprintf(out, "rae_ext_rae_math_acos");
        } else if (str_eq_cstr(name, "atan")) {
            fprintf(out, "rae_ext_rae_math_atan");
        } else if (str_eq_cstr(name, "atan2")) {
            fprintf(out, "rae_ext_rae_math_atan2");
        } else if (str_eq_cstr(name, "sqrt")) {
            fprintf(out, "rae_ext_rae_math_sqrt");
        } else if (str_eq_cstr(name, "pow")) {
            fprintf(out, "rae_ext_rae_math_pow");
        } else if (str_eq_cstr(name, "exp")) {
            fprintf(out, "rae_ext_rae_math_exp");
        } else if (str_eq_cstr(name, "math_log")) {
            fprintf(out, "rae_ext_rae_math_log");
        } else if (str_eq_cstr(name, "floor")) {
            fprintf(out, "rae_ext_rae_math_floor");
        } else if (str_eq_cstr(name, "ceil")) {
            fprintf(out, "rae_ext_rae_math_ceil");
        } else if (str_eq_cstr(name, "round")) {
            fprintf(out, "rae_ext_rae_math_round");
        } else {
            fprintf(out, "rae_ext_%.*s", (int)name.len, name.data);
        }
        return;
    }

    fprintf(out, "rae_%.*s", (int)func->name.len, func->name.data);
    
    // Non-externs always get param-based mangling to avoid any chance of collision.
    // We use a trailing underscore for each part.
    // For generic functions, we erase all parameter types to 'Any' to match the single definition.
    bool erase_generics = (func->generic_params != NULL);

    fprintf(out, "_");
    for (const AstParam* p = func->params; p; p = p->next) {
        emit_mangled_type_name_ext(p->type, out, erase_generics);
        fprintf(out, "_");
    }
}

static bool emit_call_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
  const AstExpr* callee = expr->as.call.callee;
  
  if (callee->kind == AST_EXPR_IDENT) {
      Str name = callee->as.ident;
      
      // 1. Intrinsics
      if (str_eq_cstr(name, "__buf_alloc")) {
          const AstCallArg* arg = expr->as.call.args;
          if (!arg) return false;
          fprintf(out, "rae_ext_rae_buf_alloc(");
          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST, false)) return false;
          const char* elem_size_str = "8"; // Default
          if (ctx->func_decl && ctx->func_decl->generic_params) elem_size_str = "sizeof(RaeAny)";
          else if (ctx->func_decl && (str_eq_cstr(ctx->func_decl->name, "createList2") || str_eq_cstr(ctx->func_decl->name, "grow"))) elem_size_str = "sizeof(RaeAny)";
          fprintf(out, ", %s)", elem_size_str);
          return true;
      }
      if (str_eq_cstr(name, "__buf_free")) {
          const AstCallArg* arg = expr->as.call.args;
          if (!arg) return false;
          fprintf(out, "rae_ext_rae_buf_free(");
          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST, false)) return false;
          fprintf(out, ")");
          return true;
      }
      if (str_eq_cstr(name, "__buf_resize")) {
          const AstCallArg* arg = expr->as.call.args;
          if (!arg || !arg->next) return false;
          fprintf(out, "rae_ext_rae_buf_resize(");
          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST, false)) return false;
          fprintf(out, ", ");
          if (!emit_expr(ctx, arg->next->value, out, PREC_LOWEST, false)) return false;
          const char* elem_size_str = "8";
          if (ctx->func_decl && ctx->func_decl->generic_params) elem_size_str = "sizeof(RaeAny)";
          else if (ctx->func_decl && str_eq_cstr(ctx->func_decl->name, "grow")) elem_size_str = "sizeof(RaeAny)";
          fprintf(out, ", %s)", elem_size_str);
          return true;
      }
      if (str_eq_cstr(name, "__buf_copy")) {
          const AstCallArg* arg = expr->as.call.args;
          if (!arg || !arg->next || !arg->next->next || !arg->next->next->next || !arg->next->next->next->next) return false;
          fprintf(out, "rae_ext_rae_buf_copy(");
          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST, false)) return false; // src
          fprintf(out, ", ");
          if (!emit_expr(ctx, arg->next->value, out, PREC_LOWEST, false)) return false; // src_off
          fprintf(out, ", ");
          if (!emit_expr(ctx, arg->next->next->value, out, PREC_LOWEST, false)) return false; // dst
          fprintf(out, ", ");
          if (!emit_expr(ctx, arg->next->next->next->value, out, PREC_LOWEST, false)) return false; // dst_off
          fprintf(out, ", ");
          if (!emit_expr(ctx, arg->next->next->next->next->value, out, PREC_LOWEST, false)) return false; // len
          const char* elem_size_str = "8";
          if (ctx->func_decl && ctx->func_decl->generic_params) elem_size_str = "sizeof(RaeAny)";
          else {
              Str buf_type = infer_expr_type(ctx, arg->value);
              if (str_eq_cstr(buf_type, "List") || str_eq_cstr(buf_type, "List2") || str_eq_cstr(buf_type, "Any")) elem_size_str = "sizeof(RaeAny)";
          }
          fprintf(out, ", %s)", elem_size_str);
          return true;
      }
      if (str_eq_cstr(name, "__buf_get")) {
          const AstCallArg* arg = expr->as.call.args;
          if (!arg || !arg->next) return false;
          const AstTypeRef* item_type = infer_buffer_element_type_ref(ctx, arg->value);
          fprintf(out, "((");
          if (item_type) {
              emit_type_ref_as_c_type(ctx, item_type, out);
          } else {
              fprintf(out, "int64_t");
          }
          fprintf(out, "*)( ");
          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST, false)) return false;
          fprintf(out, "))[");
          if (!emit_expr(ctx, arg->next->value, out, PREC_LOWEST, false)) return false;
          fprintf(out, "]");
          return true;
      }
      if (str_eq_cstr(name, "__buf_set")) {
          const AstCallArg* arg = expr->as.call.args;
          if (!arg || !arg->next || !arg->next->next) return false;
          const AstTypeRef* item_type = infer_buffer_element_type_ref(ctx, arg->value);
          fprintf(out, "((");
          if (item_type) {
              emit_type_ref_as_c_type(ctx, item_type, out);
          } else {
              fprintf(out, "int64_t");
          }
          fprintf(out, "*)( ");
          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST, false)) return false;
          fprintf(out, "))[");
          if (!emit_expr(ctx, arg->next->value, out, PREC_LOWEST, false)) return false;
          fprintf(out, "] = ");
          
          bool is_any = item_type && (item_type->is_opt || str_eq_cstr(get_base_type_name(item_type), "Any"));
          // Check if it's a generic parameter (which also maps to Any)
          if (item_type && !is_any) {
              Str ab = get_base_type_name(item_type);
              if (ab.len == 1 && ab.data[0] >= 'A' && ab.data[0] <= 'Z') is_any = true;
          }

          if (is_any) fprintf(out, "rae_any(");
          const AstTypeRef* old_expected = ctx->expected_type;
          ctx->expected_type = item_type;
          if (!emit_expr(ctx, arg->next->next->value, out, PREC_LOWEST, false)) return false;
          ctx->expected_type = old_expected;
          if (is_any) fprintf(out, ")");
          return true;
      }
  }

  // Find function declaration if possible for dispatch
  const AstFuncDecl* func_decl = NULL;
  
  if (callee->kind == AST_EXPR_IDENT) {
      uint16_t arg_count = 0;
      for (const AstCallArg* arg = expr->as.call.args; arg; arg = arg->next) arg_count++;
      
      Str* arg_types = NULL;
      if (arg_count > 0) {
          arg_types = malloc(arg_count * sizeof(Str));
          const AstCallArg* arg = expr->as.call.args;
          for (uint16_t i = 0; i < arg_count; ++i) {
              arg_types[i] = infer_expr_type(ctx, arg->value);
              arg = arg->next;
          }
      }
      
      func_decl = find_function_overload(ctx->module, ctx, callee->as.ident, arg_types, arg_count);
      
      // If not found with exact types, try finding any overload by name only to get a generic signature
      // Search for function declaration project-wide
      if (!func_decl) {
          for (size_t i = 0; i < g_all_decl_count; i++) {
              const AstDecl* d = g_all_decls[i];
              if (d->kind == AST_DECL_FUNC && str_eq(d->as.func_decl.name, callee->as.ident)) {
                  // TODO: Match parameter types for overloading
                  func_decl = &d->as.func_decl;
                  break;
              }
          }
      }

      free(arg_types);

      if (!func_decl && !find_raylib_mapping(callee->as.ident)) {
          // Final check: persistent registry from VM
          if (!ctx->registry || !vm_registry_find_native(ctx->registry, str_to_cstr(callee->as.ident))) {
              char buffer[128];
              snprintf(buffer, sizeof(buffer), "unknown function '%.*s' for VM call", (int)callee->as.ident.len, callee->as.ident.data);
              diag_error(ctx->module->file_path, (int)expr->line, (int)expr->column, buffer);
              return false;
          }
      }
  } else if (callee->kind == AST_EXPR_MEMBER) {
      uint16_t arg_count = 1;
      for (const AstCallArg* arg = expr->as.call.args; arg; arg = arg->next) arg_count++;
      
      Str* arg_types = malloc(arg_count * sizeof(Str));
      arg_types[0] = infer_expr_type(ctx, callee->as.member.object);
      const AstCallArg* arg = expr->as.call.args;
      for (uint16_t i = 1; i < arg_count; ++i) {
          arg_types[i] = infer_expr_type(ctx, arg->value);
          arg = arg->next;
      }
      func_decl = find_function_overload(ctx->module, ctx, callee->as.member.member, arg_types, arg_count);
      free(arg_types);
  }

  // Handle return type casting if it's a generic return
  const char* cast_pre = "";
  const char* cast_post = "";
  const char* cast_post_extra = "";
  if (func_decl && func_decl->returns && func_decl->returns->type && func_decl->returns->type->parts) {
      Str rtype = func_decl->returns->type->parts->text;
      bool is_generic_ret = func_decl->returns->type->generic_args != NULL;
      if (!is_generic_ret) {
          const AstIdentifierPart* gp = func_decl->generic_params;
          while (gp) {
              if (str_eq(gp->text, rtype)) { is_generic_ret = true; break; }
              gp = gp->next;
          }
      }
      
      if (is_generic_ret) {
          Str inferred = infer_expr_type(ctx, expr);
          
          bool is_opt_ret = false;
          if (func_decl && func_decl->returns && func_decl->returns->type && func_decl->returns->type->is_opt) {
              is_opt_ret = true;
          }

          if (is_opt_ret) {
              cast_pre = "((RaeAny)(";
              cast_post = "))";
          } else if (str_eq_cstr(inferred, "Int")) {
              cast_pre = "((int64_t)(";
              cast_post = ").as.i)";
          } else if (str_eq_cstr(inferred, "Float")) {
              cast_pre = "((double)(";
              cast_post = ").as.f)";
          } else if (str_eq_cstr(inferred, "Bool")) {
              cast_pre = "((int8_t)(";
              cast_post = ").as.b)";
          } else if (str_eq_cstr(inferred, "String")) {
              cast_pre = "((const char*)(";
              cast_post = ").as.s)";
          } else {
              // Custom type / struct pointer
              bool is_generic_param_name = false;
              if (ctx && ctx->generic_params) {
                  const AstIdentifierPart* gp = ctx->generic_params;
                  while (gp) {
                      if (str_eq(gp->text, inferred)) { is_generic_param_name = true; break; }
                      gp = gp->next;
                  }
              }

              if (str_eq_cstr(inferred, "Any") || is_generic_param_name) {
                  cast_pre = "((RaeAny)(";
                  cast_post = "))";
              } else {
                  char* expected_name = NULL;
                  if (ctx->expected_type) {
                      expected_name = mangled_type_name_to_cstr(ctx->expected_type);
                  } else {
                      expected_name = str_to_cstr(inferred);
                  }

                  bool is_generic_ret = func_decl && func_decl->returns && func_decl->returns->type && func_decl->returns->type->generic_args;
                  
                  if (is_generic_ret) {
                      char* type_name = mangled_type_name_to_cstr(func_decl->returns->type);
                      char* buf = malloc(strlen(expected_name) + strlen(type_name) + 64);
                      // Type punning via union to reinterpret generic return struct as specialized struct
                      sprintf(buf, "((union { %s src; %s dst; }){ .src = ", type_name, expected_name);
                      cast_pre = buf;
                      cast_post_extra = " }).dst";
                      free(type_name);
                  } else {
                      char* buf = malloc(strlen(expected_name) + 32);
                      sprintf(buf, "((%s*)(", expected_name);
                      cast_pre = buf;
                      cast_post = ").as.ptr)";
                  }
                  free(expected_name);
              }
          }
      }
  }

  fprintf(out, "%s", cast_pre);

  if (func_decl && !str_eq_cstr(func_decl->name, "main")) {
      emit_mangled_function_name(func_decl, out);
  } else if (callee->kind == AST_EXPR_IDENT) {
      const char* ray_mapping = find_raylib_mapping(callee->as.ident);
      if (ray_mapping) {
          fprintf(out, "rae_ext_%.*s", (int)callee->as.ident.len, callee->as.ident.data);
      } else {
          if (!emit_expr(ctx, callee, out, PREC_CALL, false)) return false;
      }
  } else {
      if (!emit_expr(ctx, callee, out, PREC_CALL, false)) return false;
  }

  if (fprintf(out, "(") < 0) return false;

  const AstCallArg* arg = expr->as.call.args;
  AstCallArg this_arg = {0};
  if (callee->kind == AST_EXPR_MEMBER) {
      this_arg.value = callee->as.member.object;
      this_arg.next = (AstCallArg*)expr->as.call.args;
      arg = &this_arg;
  }

  const AstParam* param = func_decl ? func_decl->params : NULL;
  bool first = true;
  
  while (arg) {
    if (!first) {
      if (fprintf(out, ", ") < 0) return false;
    }
    first = false;
    
    const AstTypeRef* old_expected = ctx->expected_type;
    ctx->expected_type = param ? param->type : NULL;

    bool needs_addr = false;
    bool is_any_param = false;
    
    if (func_decl) {
        // We have signature!
        if (param) {
             // SEMANTICS:
             // needs_addr is true if:
             // 1. Explicitly mod/view
             // 2. Default (implicit view) AND not primitive AND not explicitly val
             
             if (param->type && param->type->parts) {
                 Str ptype = param->type->parts->text;
                 if (str_eq_cstr(ptype, "Any")) {
                     is_any_param = true;
                 } else if (func_decl->generic_params) {
                     const AstIdentifierPart* gp = func_decl->generic_params;
                     while (gp) {
                         if (str_eq(gp->text, ptype)) {
                             is_any_param = true;
                             break;
                         }
                         gp = gp->next;
                     }
                 }
             }

             if (param->type) {
                 bool is_mod = param->type->is_mod;
                 bool is_explicit_view = param->type->is_view;
                 bool is_val = param->type->is_val;
                 bool is_primitive = false;
                 if (param->type->parts) {
                     Str ptype = param->type->parts->text;
                     is_primitive = is_primitive_type(ptype) || (ctx->uses_raylib && is_raylib_builtin_type(ptype));
                     if (!is_primitive && find_enum_decl(ctx->module, ptype)) {
                         is_primitive = true;
                     }
                 }
                 
                 if (is_mod || is_explicit_view || (!is_val && !is_primitive)) {
                     needs_addr = true;
                 }
                 
                 // SPECIAL CASE: RaeAny is a value type even if it's a generic T
                 if (is_any_param) {
                     needs_addr = false;
                 }
             }
             // We'll advance 'param' at the end of the loop to be safe with all branches
        }
    } else {
        // Fallback heuristic: assume view-by-default for non-primitives
        Str arg_type_name = infer_expr_type(ctx, arg->value);
        if (arg_type_name.len > 0 && !is_primitive_type(arg_type_name)) {
            // Also skip if it's an enum
            bool is_enum = find_enum_decl(ctx->module, arg_type_name) != NULL;
            if (!is_enum) {
                needs_addr = true;
            }
        }
    }

    bool have_ptr = false;
    if (arg->value->kind == AST_EXPR_IDENT) {
        if (is_pointer_type(ctx, arg->value->as.ident)) {
            have_ptr = true;
        }
    } else if (arg->value->kind == AST_EXPR_UNARY && (arg->value->as.unary.op == AST_UNARY_VIEW || arg->value->as.unary.op == AST_UNARY_MOD)) {
        have_ptr = true; 
    }
    
    if (needs_addr && !have_ptr) {
         // Special case: if it's a literal, we need a compound literal to take the address of a temporary
         if (arg->value->kind == AST_EXPR_STRING || arg->value->kind == AST_EXPR_INTEGER || 
             arg->value->kind == AST_EXPR_FLOAT || arg->value->kind == AST_EXPR_BOOL) {
             
             const char* c_type = "int64_t";
             if (arg->value->kind == AST_EXPR_STRING) c_type = "const char*";
             else if (arg->value->kind == AST_EXPR_FLOAT) c_type = "double";
             else if (arg->value->kind == AST_EXPR_BOOL) c_type = "int8_t";
             
             fprintf(out, "&((%s){ ", c_type);
             if (!emit_expr(ctx, arg->value, out, PREC_LOWEST, false)) return false;
             fprintf(out, " })");
             
             // Advance to next argument skipping normal emission
             ctx->expected_type = old_expected;
             arg = arg->next;
             if (func_decl && param) param = param->next;
             continue;
         }
         
         if (param && param->type && param->type->generic_args) {
             char* cast_type = mangled_type_name_to_cstr(param->type);
             fprintf(out, "((%s*)", cast_type);
             free(cast_type);
         }
         if (fprintf(out, "&(") < 0) return false;
    } else if (!needs_addr && have_ptr) {
         if (fprintf(out, "(*") < 0) return false;
    }

    Str arg_type = infer_expr_type(ctx, arg->value);
    Str target_type = arg_type;
    if (target_type.len == 0 && param && param->type && param->type->parts) {
        target_type = param->type->parts->text;
    }
    
    bool is_c_struct = false;
    const AstDecl* type_decl = find_type_decl(ctx->module, target_type);
    if (type_decl && type_decl->kind == AST_DECL_TYPE && has_property(type_decl->as.type_decl.properties, "c_struct")) {
        is_c_struct = true;
    } else if (str_eq_cstr(target_type, "Vector2") || str_eq_cstr(target_type, "Vector3") || 
               str_eq_cstr(target_type, "Color") || str_eq_cstr(target_type, "Camera3D")) {
        is_c_struct = true;
    }

    bool arg_is_any = str_eq_cstr(arg_type, "Any");
    bool arg_is_opt = false;
    if (arg->value->kind == AST_EXPR_IDENT) {
        const AstTypeRef* tr = get_local_type_ref(ctx, arg->value->as.ident);
        if (tr && tr->is_opt) arg_is_opt = true;
    }

    if (is_any_param && !arg_is_any && !arg_is_opt) {
        fprintf(out, "rae_any(");
    } else if (!is_any_param && (arg_is_any || arg_is_opt)) {
            // Unwrapping Any/opt to concrete type
            if (needs_addr) {
                // reference (ptr)
                if (target_type.len > 0) {
                    fprintf(out, "((%.*s*)(", (int)target_type.len, target_type.data);
                } else {
                    fprintf(out, "((void*)(");
                }
            } else {
                // value
                if (str_eq_cstr(target_type, "Int")) {
                    fprintf(out, "((int64_t)(");
                } else if (str_eq_cstr(target_type, "Float")) {
                    fprintf(out, "((double)(");
                } else if (str_eq_cstr(target_type, "Bool")) {
                    fprintf(out, "((int8_t)(");
                } else if (str_eq_cstr(target_type, "String")) {
                    fprintf(out, "((const char*)(");
                } else {
                    fprintf(out, "((void*)("); // fallback
                }
            }
        }

    if (is_c_struct && !needs_addr && !is_any_param) {
        // If it's already an object of the right type (explicitly tagged), just emit it
        if (arg->value->kind == AST_EXPR_OBJECT && arg->value->as.object_literal.type) {
            if (!emit_expr(ctx, arg->value, out, PREC_LOWEST, false)) return false;
        } else if (arg->value->kind == AST_EXPR_OBJECT) {
            // Fix: All native calls with untyped object literals MUST have a cast: (Type){...}
            fprintf(out, "(%.*s){ ", (int)target_type.len, target_type.data);
            if (type_decl) {
                const AstTypeField* field = type_decl->as.type_decl.fields;
                while (field) {
                    fprintf(out, ".%.*s = ", (int)field->name.len, field->name.data);
                    bool found_field = false;
                    const AstObjectField* of = arg->value->as.object_literal.fields;
                    while (of) {
                        if (str_eq(of->name, field->name)) {
                            if (!emit_expr(ctx, of->value, out, PREC_LOWEST, false)) return false;
                            found_field = true;
                            break;
                        }
                        of = of->next;
                    }
                    if (!found_field) fprintf(out, "0");
                    if (field->next) fprintf(out, ", ");
                    field = field->next;
                }
            } else {
                // Fallback for untyped object literal mapping to unknown (but known c_struct) type
                const AstObjectField* of = arg->value->as.object_literal.fields;
                while (of) {
                    fprintf(out, ".%.*s = ", (int)of->name.len, of->name.data);
                    if (!emit_expr(ctx, of->value, out, PREC_LOWEST, false)) return false;
                    if (of->next) fprintf(out, ", ");
                    of = of->next;
                }
            }
            fprintf(out, " }");
        } else {
            // It's a variable or other expression of the same/compatible type.
            // C allows implicit conversion between structs if they are the SAME type,
            // or we might need a cast if names differ but layout matches.
            // For now, we assume Rae type matches C type name.
            if (!emit_expr(ctx, arg->value, out, PREC_LOWEST, false)) return false;
        }
    } else {
        if (!emit_expr(ctx, arg->value, out, PREC_LOWEST, false)) {
          return false;
        }
    }

    if (is_any_param && !arg_is_any && !arg_is_opt) {
        fprintf(out, ")");
    } else if (!is_any_param && (arg_is_any || arg_is_opt)) {
        if (needs_addr) {
            fprintf(out, ").as.ptr)");
        } else {
            if (str_eq_cstr(target_type, "Int")) {
                fprintf(out, ").as.i)");
            } else if (str_eq_cstr(target_type, "Float")) {
                fprintf(out, ").as.f)");
            } else if (str_eq_cstr(target_type, "Bool")) {
                fprintf(out, ").as.b)");
            } else if (str_eq_cstr(target_type, "String")) {
                fprintf(out, ").as.s)");
            } else {
                fprintf(out, ").as.ptr)");
            }
        }
    }

    if (needs_addr && !have_ptr) {
         if (fprintf(out, ")") < 0) return false;
         if (param && param->type && param->type->generic_args) fprintf(out, ")");
    } else if (!needs_addr && have_ptr) {
         if (fprintf(out, ")") < 0) return false;
    }
    
    ctx->expected_type = old_expected;
    arg = arg->next;
    if (func_decl && param) param = param->next;
  }
  fprintf(out, ")");
  fprintf(out, "%s", cast_post);
  fprintf(out, "%s", cast_post_extra);
  return true;
}

static bool is_pointer_type(CFuncContext* ctx, Str name) {
  for (const AstParam* param = ctx->params; param; param = param->next) {
    if (str_eq(param->name, name)) {
        if (param->type) {
            if (param->type->is_view || param->type->is_mod) return true;
            if (param->type->parts && str_eq_cstr(param->type->parts->text, "Buffer")) return true;
        }
    }
  }
  for (size_t i = 0; i < ctx->local_count; ++i) {
    if (str_eq(ctx->locals[i], name)) {
      if (ctx->local_is_ptr[i]) return true;
      if (str_eq_cstr(ctx->local_types[i], "Buffer")) return true;
    }
  }
  return false;
}

static bool is_mod_type(CFuncContext* ctx, Str name) {
  for (const AstParam* param = ctx->params; param; param = param->next) {
    if (str_eq(param->name, name)) {
        if (param->type) {
            return param->type->is_mod;
        }
    }
  }
  for (size_t i = 0; i < ctx->local_count; ++i) {
    if (str_eq(ctx->locals[i], name)) {
      return ctx->local_is_mod[i];
    }
  }
  return false;
}

static Str get_local_type_name(CFuncContext* ctx, Str name) {
  for (const AstParam* param = ctx->params; param; param = param->next) {
    if (str_eq(param->name, name)) {
        if (param->type && param->type->parts) return param->type->parts->text;
    }
  }
  for (size_t i = 0; i < ctx->local_count; ++i) {
    if (str_eq(ctx->locals[i], name)) {
      return ctx->local_types[i];
    }
  }
  return (Str){0};
}

static const AstTypeRef* get_local_type_ref(CFuncContext* ctx, Str name) {
  for (const AstParam* param = ctx->params; param; param = param->next) {
    if (str_eq(param->name, name)) return param->type;
  }
  for (size_t i = 0; i < ctx->local_count; ++i) {
    if (str_eq(ctx->locals[i], name)) return ctx->local_type_refs[i];
  }
  return NULL;
}

static bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out, int parent_prec, bool is_lvalue) {
  if (!expr) return true;
  
  switch (expr->kind) {
    case AST_EXPR_STRING:
      return emit_string_literal(out, expr->as.string_lit);
    case AST_EXPR_INTERP: {
      AstInterpPart* part = expr->as.interp.parts;
      if (!part) {
          return fprintf(out, "\"\"") >= 0;
      }
      
      // We'll emit nested rae_str_concat calls
      size_t count = 0;
      AstInterpPart* curr = part;
      while (curr) { count++; curr = curr->next; }
      
      // (count - 1) concats needed
      for (size_t i = 0; i < count - 1; i++) {
          fprintf(out, "rae_ext_rae_str_concat(");
      }
      
      // Initial part
      if (part->value->kind != AST_EXPR_STRING) {
          Str type_name = infer_expr_type(ctx, part->value);
          bool is_enum = find_enum_decl(ctx->module, type_name) != NULL;
          fprintf(out, "rae_ext_rae_str(");
          if (is_enum) fprintf(out, "(int64_t)(");
          if (!emit_expr(ctx, part->value, out, PREC_LOWEST, false)) return false;
          if (is_enum) fprintf(out, ")");
          fprintf(out, ")");
      } else {
          if (!emit_expr(ctx, part->value, out, PREC_LOWEST, false)) return false;
      }
      part = part->next;
      
      while (part) {
          fprintf(out, ", ");
          if (part->value->kind != AST_EXPR_STRING) {
              Str type_name = infer_expr_type(ctx, part->value);
              bool is_enum = find_enum_decl(ctx->module, type_name) != NULL;
              fprintf(out, "rae_ext_rae_str(");
              if (is_enum) fprintf(out, "(int64_t)(");
              if (!emit_expr(ctx, part->value, out, PREC_LOWEST, false)) return false;
              if (is_enum) fprintf(out, ")");
              fprintf(out, ")");
          } else {
              if (!emit_expr(ctx, part->value, out, PREC_LOWEST, false)) return false;
          }
          fprintf(out, ")"); // close rae_ext_rae_str_concat
          part = part->next;
      }
      return true;
    }
    case AST_EXPR_CHAR: {
      int64_t c = expr->as.char_value;
      if (c >= 32 && c <= 126 && c != '\'' && c != '\\') {
          return fprintf(out, "'%c'", (char)c) >= 0;
      } else {
          return fprintf(out, "%lld", (long long)c) >= 0;
      }
    }
    case AST_EXPR_INTEGER:
      return fprintf(out, "%.*s", (int)expr->as.integer.len, expr->as.integer.data) >= 0;
    case AST_EXPR_FLOAT:
      return fprintf(out, "%.*s", (int)expr->as.floating.len, expr->as.floating.data) >= 0;
    case AST_EXPR_BOOL:
      return fprintf(out, "%d", expr->as.boolean ? 1 : 0) >= 0;
    case AST_EXPR_IDENT: {
      const AstTypeRef* tr = get_local_type_ref(ctx, expr->as.ident);
      if (is_lvalue) {
          if (tr && tr->is_view) {
              char msg[256];
              snprintf(msg, sizeof(msg), "cannot assign to read-only view identifier '%.*s'", (int)expr->as.ident.len, expr->as.ident.data);
              diag_error(ctx->module->file_path, (int)expr->line, (int)expr->column + 2, msg);
          }
      }
      for (const AstParam* param = ctx->params; param; param = param->next) {
        if (str_eq(param->name, expr->as.ident)) {
          fprintf(out, "%.*s", (int)expr->as.ident.len, expr->as.ident.data);
          return true;
        }
      }
      for (size_t i = 0; i < ctx->local_count; ++i) {
        if (str_eq(ctx->locals[i], expr->as.ident)) {
          fprintf(out, "%.*s", (int)expr->as.ident.len, expr->as.ident.data);
          return true;
        }
      }
      
      // Check if it's a function (for function pointers or just to be safe)
      const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, expr->as.ident, NULL, 0);
      if (fd) {
          emit_mangled_function_name(fd, out);
          return true;
      }

      fprintf(out, "%.*s", (int)expr->as.ident.len, expr->as.ident.data);
      return true;
    }
    case AST_EXPR_BINARY: {
      Str lhs_type = infer_expr_type(ctx, expr->as.binary.lhs);
      bool lhs_is_any = str_eq_cstr(lhs_type, "Any");
      
      if (expr->as.binary.op == AST_BIN_IS && expr->as.binary.rhs->kind == AST_EXPR_NONE) {
          if (lhs_is_any) {
              fprintf(out, "(");
              if (!emit_expr(ctx, expr->as.binary.lhs, out, PREC_LOWEST, false)) return false;
              fprintf(out, ".type == RAE_TYPE_NONE)");
              return true;
          }
      }

      int prec = binary_op_precedence(expr->as.binary.op);
      bool need_paren = prec < parent_prec;
      const char* op = NULL;
      switch (expr->as.binary.op) {
        case AST_BIN_ADD: op = "+"; break;
        case AST_BIN_SUB: op = "-"; break;
        case AST_BIN_MUL: op = "*"; break;
        case AST_BIN_DIV: op = "/"; break;
        case AST_BIN_MOD: {
            Str ltype = infer_expr_type(ctx, expr->as.binary.lhs);
            Str rtype = infer_expr_type(ctx, expr->as.binary.rhs);
            if (str_eq_cstr(ltype, "Float") || str_eq_cstr(rtype, "Float")) {
                fprintf(out, "fmod(");
                if (!emit_expr(ctx, expr->as.binary.lhs, out, PREC_LOWEST, false)) return false;
                fprintf(out, ", ");
                if (!emit_expr(ctx, expr->as.binary.rhs, out, PREC_LOWEST, false)) return false;
                fprintf(out, ")");
            } else {
                if (need_paren) fprintf(out, "(");
                if (!emit_expr(ctx, expr->as.binary.lhs, out, prec, false)) return false;
                fprintf(out, " %% ");
                if (!emit_expr(ctx, expr->as.binary.rhs, out, prec + 1, false)) return false;
                if (need_paren) fprintf(out, ")");
            }
            return true;
        }
        case AST_BIN_LT: op = "<"; break;
        case AST_BIN_GT: op = ">"; break;
        case AST_BIN_LE: op = "<="; break;
        case AST_BIN_GE: op = ">="; break;
        case AST_BIN_IS: op = "=="; break;
        case AST_BIN_AND: op = "&&"; break;
        case AST_BIN_OR: op = "||"; break;
        default:
          fprintf(stderr, "error: C backend does not support this binary operator yet\n");
          return false;
      }
      if (expr->as.binary.op == AST_BIN_IS) {
          bool use_strcmp = is_string_expr(expr->as.binary.lhs) || is_string_expr(expr->as.binary.rhs);
          if (!use_strcmp && expr->as.binary.lhs->kind == AST_EXPR_IDENT) {
              Str type = get_local_type_name(ctx, expr->as.binary.lhs->as.ident);
              if (str_eq_cstr(type, "String")) use_strcmp = true;
          }
          
          if (use_strcmp) {
              if (fprintf(out, "(strcmp((rae_ext_rae_str(") < 0) return false;
              if (!emit_expr(ctx, expr->as.binary.lhs, out, prec, false)) return false;
              if (fprintf(out, ") ? rae_ext_rae_str(") < 0) return false;
              if (!emit_expr(ctx, expr->as.binary.lhs, out, prec, false)) return false;
              if (fprintf(out, ") : \"\"), (rae_ext_rae_str(") < 0) return false;
              if (!emit_expr(ctx, expr->as.binary.rhs, out, prec, false)) return false;
              if (fprintf(out, ") ? rae_ext_rae_str(") < 0) return false;
              if (!emit_expr(ctx, expr->as.binary.rhs, out, prec, false)) return false;
              if (fprintf(out, ") : \"\")) == 0)") < 0) return false;
              return true;
          }
      }

      if (need_paren && fprintf(out, "(") < 0) return false;
      
      bool lhs_is_ptr = (expr->as.binary.lhs->kind == AST_EXPR_IDENT && is_pointer_type(ctx, expr->as.binary.lhs->as.ident));
      if (lhs_is_ptr) fprintf(out, "(*");
      if (!emit_expr(ctx, expr->as.binary.lhs, out, prec, false)) return false;
      if (lhs_is_ptr) fprintf(out, ")");
      
      if (fprintf(out, " %s ", op) < 0) return false;
      
      bool rhs_is_ptr = (expr->as.binary.rhs->kind == AST_EXPR_IDENT && is_pointer_type(ctx, expr->as.binary.rhs->as.ident));
      if (rhs_is_ptr) fprintf(out, "(*");
      if (!emit_expr(ctx, expr->as.binary.rhs, out, prec + 1, false)) return false;
      if (rhs_is_ptr) fprintf(out, ")");
      
      if (need_paren && fprintf(out, ")") < 0) return false;
      return true;
    }
    case AST_EXPR_UNARY:
      if (expr->as.unary.op == AST_UNARY_NEG) {
        if (fprintf(out, "(-") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false)) return false;
        return fprintf(out, ")") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_NOT) {
        if (fprintf(out, "(!(") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false)) return false;
        return fprintf(out, "))") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_PRE_INC) {
        if (fprintf(out, "++") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true)) return false;
        return true;
      } else if (expr->as.unary.op == AST_UNARY_PRE_DEC) {
        if (fprintf(out, "--") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true)) return false;
        return true;
      } else if (expr->as.unary.op == AST_UNARY_POST_INC) {
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true)) return false;
        return fprintf(out, "++") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_POST_DEC) {
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true)) return false;
        return fprintf(out, "--") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_SPAWN) {
          const AstExpr* call = expr->as.unary.operand;
          if (call->kind != AST_EXPR_CALL || call->as.call.callee->kind != AST_EXPR_IDENT) {
              fprintf(stderr, "error: spawn must be followed by a function call\n");
              return false;
          }
          
          Str name = call->as.call.callee->as.ident;
          uint16_t arg_count = 0;
          for (const AstCallArg* arg = call->as.call.args; arg; arg = arg->next) arg_count++;
          
          Str* arg_types = NULL;
          if (arg_count > 0) {
              arg_types = malloc(arg_count * sizeof(Str));
              const AstCallArg* arg = call->as.call.args;
              for (uint16_t i = 0; i < arg_count; ++i) {
                  arg_types[i] = infer_expr_type(ctx, arg->value);
                  arg = arg->next;
              }
          }
          
          const AstFuncDecl* func = find_function_overload(ctx->module, ctx, name, arg_types, arg_count);
          if (arg_types) free(arg_types);
          
          if (!func) {
              fprintf(stderr, "error: could not find function for spawn\n");
              return false;
          }

          fprintf(out, "(void)__extension__ ({ ");
          fprintf(out, "_spawn_args_");
          emit_mangled_function_name(func, out);
          fprintf(out, "* _args = malloc(sizeof(*_args)); ");
          
          const AstCallArg* carg = call->as.call.args;
          const AstParam* p = func->params;
          while (carg && p) {
              fprintf(out, "_args->%.*s = ", (int)p->name.len, p->name.data);
              if (!emit_expr(ctx, carg->value, out, PREC_LOWEST, false)) return false;
              fprintf(out, "; ");
              carg = carg->next;
              p = p->next;
          }
          
          fprintf(out, "rae_spawn(_spawn_wrapper_");
          emit_mangled_function_name(func, out);
          fprintf(out, ", _args); ");
          fprintf(out, "0; })");
          return true;
      } else if (expr->as.unary.op == AST_UNARY_VIEW || expr->as.unary.op == AST_UNARY_MOD) {
        // In C backend, references are pointers. We take the address of the operand.
        if (fprintf(out, "(&(") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false)) return false;
        return fprintf(out, "))") >= 0;
      }
      fprintf(stderr, "error: C backend unsupported unary operator\n");
      return false;
    case AST_EXPR_MATCH: {
      bool use_string = false;
      if (!match_arms_use_string(expr->as.match_expr.arms, &use_string)) {
        return false;
      }
      
      bool is_ptr = false;
      if (expr->as.match_expr.subject->kind == AST_EXPR_IDENT) {
          if (is_pointer_type(ctx, expr->as.match_expr.subject->as.ident)) {
              is_ptr = true;
          }
      }

      // Determine return type of match expression (heuristic)
      const char* res_type = "int64_t";
      const AstMatchArm* arm_ptr = expr->as.match_expr.arms;
      while (arm_ptr) {
          if (arm_ptr->value->kind == AST_EXPR_STRING) {
              res_type = "const char*";
              break;
          }
          arm_ptr = arm_ptr->next;
      }

      const char* match_type = "int64_t";
      if (use_string) match_type = "const char*";
      else if (is_ptr) match_type = "void*";

      size_t temp_id = ctx->temp_counter++;
      if (fprintf(out, "__extension__ ({ %s __match%zu = ", match_type, temp_id) < 0) {
        return false;
      }
      if (!emit_expr(ctx, expr->as.match_expr.subject, out, PREC_LOWEST, false)) {
        return false;
      }
      if (fprintf(out, "; %s __result%zu; ", res_type, temp_id) < 0) {
        return false;
      }
      const AstMatchArm* arm = expr->as.match_expr.arms;
      bool has_default = false;
      while (arm) {
        if (!arm->pattern) {
          has_default = true;
          break;
        }
        arm = arm->next;
      }
      if (!has_default) {
        fprintf(stderr, "error: match expression requires '_' default arm\n");
        return false;
      }
      arm = expr->as.match_expr.arms;
      bool first = true;
      while (arm) {
        if (!arm->pattern) {
          if (fprintf(out, "%s{ __result%zu = ", first ? "" : " else ", temp_id) < 0) {
            return false;
          }
          if (!emit_expr(ctx, arm->value, out, PREC_LOWEST, false)) {
            return false;
          }
          if (fprintf(out, "; } ") < 0) {
            return false;
          }
        } else {
          if (use_string) {
            if (fprintf(out, "%sif (__match%zu && strcmp(__match%zu, ", first ? "" : " else ", temp_id, temp_id) < 0) {
              return false;
            }
          } else {
            if (fprintf(out, "%sif (__match%zu == ", first ? "" : " else ", temp_id) < 0) {
              return false;
            }
          }
          if (!emit_expr(ctx, arm->pattern, out, PREC_LOWEST, false)) {
            return false;
          }
          if (use_string) {
            if (fprintf(out, ") == 0) { __result%zu = ", temp_id) < 0) {
              return false;
            }
          } else {
            if (fprintf(out, ") { __result%zu = ", temp_id) < 0) {
              return false;
            }
          }
          if (!emit_expr(ctx, arm->value, out, PREC_LOWEST, false)) {
            return false;
          }
          if (fprintf(out, "; } ") < 0) {
            return false;
          }
        }
        first = false;
        arm = arm->next;
      }
      if (fprintf(out, "__result%zu; })", temp_id) < 0) {
        return false;
      }
      return true;
    }
    case AST_EXPR_CALL:
      return emit_call_expr(ctx, expr, out);
    case AST_EXPR_NONE:
      return fprintf(out, "0") >= 0; // none as 0 for now
    case AST_EXPR_MEMBER: {
      if (is_lvalue && expr->as.member.object->kind == AST_EXPR_IDENT) {
          const AstTypeRef* tr = get_local_type_ref(ctx, expr->as.member.object->as.ident);
          if (tr && tr->is_view) {
              char msg[256];
              snprintf(msg, sizeof(msg), "cannot mutate field of read-only view '%.*s'", (int)expr->as.member.object->as.ident.len, expr->as.member.object->as.ident.data);
              diag_error(ctx->module->file_path, (int)expr->line, (int)expr->column + 2, msg);
          }
      }
      if (expr->as.member.object->kind == AST_EXPR_IDENT) {
          Str name = expr->as.member.object->as.ident;
          const AstDecl* ed = find_enum_decl(ctx->module, name);
          if (ed) {
              return fprintf(out, "%.*s_%.*s", (int)name.len, name.data,
                             (int)expr->as.member.member.len, expr->as.member.member.data) >= 0;
          }
      }

      const char* sep = ".";
      bool is_any = false;
      if (expr->as.member.object->kind == AST_EXPR_IDENT) {
          if (is_pointer_type(ctx, expr->as.member.object->as.ident)) {
              sep = "->";
          }
          Str type_name = get_local_type_name(ctx, expr->as.member.object->as.ident);
          if (str_eq_cstr(type_name, "Any")) is_any = true;
      }
      
      if (is_any) {
          Str inferred = infer_expr_type(ctx, expr);
          const char* union_field = "ptr";
          if (str_eq_cstr(inferred, "Int")) union_field = "i";
          else if (str_eq_cstr(inferred, "Float")) union_field = "f";
          else if (str_eq_cstr(inferred, "Bool")) union_field = "b";
          else if (str_eq_cstr(inferred, "String")) union_field = "s";
          
          fprintf(out, "((");
          if (!emit_expr(ctx, expr->as.member.object, out, PREC_CALL, false)) return false;
          fprintf(out, ")%sas.%s)", sep, union_field);
          return true;
      }

      if (!emit_expr(ctx, expr->as.member.object, out, PREC_CALL, false)) return false;
      return fprintf(out, "%s%.*s", sep, (int)expr->as.member.member.len, expr->as.member.member.data) >= 0;
    }
    case AST_EXPR_OBJECT: {
      const AstTypeDecl* type_decl = NULL;
      if (expr->as.object_literal.type && expr->as.object_literal.type->parts) {
        fprintf(out, "(");
        if (!emit_type_ref_as_c_type(ctx, expr->as.object_literal.type, out)) return false;
        fprintf(out, ")");
        
        Str type_name = expr->as.object_literal.type->parts->text;
        const AstDecl* d = find_type_decl(ctx->module, type_name);
        if (d && d->kind == AST_DECL_TYPE) type_decl = &d->as.type_decl;
      } else {
          const AstTypeRef* type_hint = ctx->expected_type;
          if (type_hint && type_hint->parts) {
              fprintf(out, "(");
              if (!emit_type_ref_as_c_type(ctx, type_hint, out)) return false;
              fprintf(out, ")");
              
              Str type_name = type_hint->parts->text;
              const AstDecl* d = find_type_decl(ctx->module, type_name);
              if (d && d->kind == AST_DECL_TYPE) type_decl = &d->as.type_decl;
          }
          
          if (!type_decl) {
              // If no explicit type, try to infer it from the expression itself (e.g. via expected type in context)
              const AstTypeRef* saved_expected = ctx->expected_type;
              ctx->expected_type = NULL;
              Str inferred = infer_expr_type(ctx, expr);
              ctx->expected_type = saved_expected;
              if (inferred.len > 0) {
                  const AstDecl* d = find_type_decl(ctx->module, inferred);
                  if (d && d->kind == AST_DECL_TYPE) type_decl = &d->as.type_decl;
              }
          }
      }
      
      if (fprintf(out, "{ ") < 0) return false;
      
      if (type_decl) {
          const AstTypeField* tf = type_decl->fields;
          const AstTypeRef* saved_expected = ctx->expected_type;
          while (tf) {
              ctx->expected_type = tf->type;
              if (fprintf(out, ".%.*s = ", (int)tf->name.len, tf->name.data) < 0) {
                  ctx->expected_type = saved_expected;
                  return false;
              }
              
              bool is_any_field = false;
              if (tf->type && tf->type->parts) {
                  Str ftype = tf->type->parts->text;
                  if (str_eq_cstr(ftype, "Any")) is_any_field = true;
                  else if (type_decl) {
                      const AstIdentifierPart* gp = type_decl->generic_params;
                      while (gp) {
                          if (str_eq(gp->text, ftype)) {
                              is_any_field = true;
                              break;
                          }
                          gp = gp->next;
                      }
                  }
              }

              const AstObjectField* f = expr->as.object_literal.fields;
              bool found = false;
              while (f) {
                  if (str_eq(f->name, tf->name)) {
                      if (is_any_field) fprintf(out, "rae_any(");
                      if (!emit_expr(ctx, f->value, out, PREC_LOWEST, false)) {
                          ctx->expected_type = saved_expected;
                          return false;
                      }
                      if (is_any_field) fprintf(out, ")");
                      found = true;
                      break;
                  }
                  f = f->next;
              }
              
              if (!found) {
                  if (tf->default_value) {
                      if (is_any_field) fprintf(out, "rae_any(");
                      if (!emit_expr(ctx, tf->default_value, out, PREC_LOWEST, false)) {
                          ctx->expected_type = saved_expected;
                          return false;
                      }
                      if (is_any_field) fprintf(out, ")");
                  } else {
                      if (is_any_field) fprintf(out, "rae_any_none()");
                      else {
                          bool is_primitive = false;
                          if (tf->type && tf->type->parts) {
                              is_primitive = is_primitive_type(tf->type->parts->text) || str_eq_cstr(tf->type->parts->text, "key");
                          }
                          if (is_primitive) fprintf(out, "0");
                          else fprintf(out, "{0}");
                      }
                  }
              }
              
              if (tf->next) fprintf(out, ", ");
              tf = tf->next;
          }
          ctx->expected_type = saved_expected;
      } else {
          const AstObjectField* field = expr->as.object_literal.fields;
          while (field) {
            if (field->name.len > 0) {
                if (fprintf(out, ".%.*s = ", (int)field->name.len, field->name.data) < 0) return false;
            }
            if (!emit_expr(ctx, field->value, out, PREC_LOWEST, false)) return false;
            if (field->next) fprintf(out, ", ");
            field = field->next;
          }
          if (!expr->as.object_literal.fields) fprintf(out, "0");
      }
      return fprintf(out, " }") >= 0;
    }
    case AST_EXPR_LIST: {
      uint16_t element_count = 0;
      AstExprList* current = expr->as.list;
      while (current) { element_count++; current = current->next; }
      
          Str int_type = str_from_cstr("Int");
          Str add_types[] = { str_from_cstr("List"), str_from_cstr("Any") };
          const AstFuncDecl* create_fn = find_function_overload(ctx->module, ctx, str_from_cstr("createList"), &int_type, 1);
          const AstFuncDecl* add_fn = find_function_overload(ctx->module, ctx, str_from_cstr("add"), add_types, 2);
      fprintf(out, "__extension__ ({ List _l = ");
      if (create_fn) emit_mangled_function_name(create_fn, out);
      else fprintf(out, "rae_createList_Int_");
      fprintf(out, "(%u); ", element_count);

      current = expr->as.list;
      while (current) {
        if (add_fn) {
            emit_mangled_function_name(add_fn, out);
        } else {
            fprintf(out, "rae_add_List_T_");
        }
        
        fprintf(out, "(&_l, rae_any(");
        if (!emit_expr(ctx, current->value, out, PREC_LOWEST, false)) return false;
        fprintf(out, ")); ");
        current = current->next;
      }
      fprintf(out, "_l; })");
      return true;
    }
    case AST_EXPR_INDEX: {
      if (is_lvalue && expr->as.index.target->kind == AST_EXPR_IDENT) {
          const AstTypeRef* tr = get_local_type_ref(ctx, expr->as.index.target->as.ident);
          if (tr && tr->is_view) {
              char msg[256];
              snprintf(msg, sizeof(msg), "cannot mutate read-only view '%.*s' via indexing", (int)expr->as.index.target->as.ident.len, expr->as.index.target->as.ident.data);
              diag_error(ctx->module->file_path, (int)expr->line, (int)expr->column + 2, msg);
          }
      }
      Str target_type = infer_expr_type(ctx, expr->as.index.target);
      Str param_types[] = { target_type, str_from_cstr("Int") };
      const AstFuncDecl* func_decl = find_function_overload(ctx->module, ctx, str_from_cstr("get"), param_types, 2);

      if (func_decl) {
          emit_mangled_function_name(func_decl, out);
          fprintf(out, "(");
          
          bool needs_addr = false;
          if (func_decl->params && (func_decl->params->type->is_view || func_decl->params->type->is_mod)) {
              needs_addr = true;
          }
          
          if (needs_addr) fprintf(out, "&(");
          if (!emit_expr(ctx, expr->as.index.target, out, PREC_LOWEST, false)) return false;
          if (needs_addr) fprintf(out, ")");
          
          fprintf(out, ", ");
          if (!emit_expr(ctx, expr->as.index.index, out, PREC_LOWEST, false)) return false;
          fprintf(out, ")");
          return true;
      }
      
      return false;
    }
    case AST_EXPR_METHOD_CALL: {
      Str method = expr->as.method_call.method_name;
      
      uint16_t explicit_args_count = 0;
      for (const AstCallArg* arg = expr->as.method_call.args; arg; arg = arg->next) explicit_args_count++;
      uint16_t total_arg_count = 1 + explicit_args_count;

      Str* arg_types = malloc(total_arg_count * sizeof(Str));
      arg_types[0] = get_base_type_name_str(infer_expr_type(ctx, expr->as.method_call.object));
      {
          const AstCallArg* arg = expr->as.method_call.args;
          for (uint16_t i = 0; i < explicit_args_count; ++i) {
              arg_types[i + 1] = infer_expr_type(ctx, arg->value);
              arg = arg->next;
          }
      }

      const AstFuncDecl* func_decl = find_function_overload(ctx->module, ctx, method, arg_types, total_arg_count);
      free(arg_types);

      if (!func_decl && str_eq_cstr(method, "toJson") && explicit_args_count == 0) {
          Str type = get_base_type_name_str(infer_expr_type(ctx, expr->as.method_call.object));
          const AstDecl* td = find_type_decl(ctx->module, type);
          if (td && td->kind == AST_DECL_TYPE) {
              fprintf(out, "rae_toJson_%.*s_(", (int)type.len, type.data);
              bool is_ptr = false;
              if (expr->as.method_call.object->kind == AST_EXPR_IDENT) {
                  is_ptr = is_pointer_type(ctx, expr->as.method_call.object->as.ident);
              }
              if (is_ptr) {
                  if (!emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, false)) return false;
              } else {
                  fprintf(out, "&(");
                  if (!emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, false)) return false;
                  fprintf(out, ")");
              }
              fprintf(out, ")");
              return true;
          }
      }

      if (!func_decl && str_eq_cstr(method, "fromJson") && explicit_args_count == 1) {
          // Note: in Rae we use T.fromJson(json), but desugared it might look like obj.fromJson(json)
          // Actually, we want to support static-like calls: Type.fromJson(json)
          if (expr->as.method_call.object->kind == AST_EXPR_IDENT) {
              Str type_name = expr->as.method_call.object->as.ident;
              const AstDecl* td = find_type_decl(ctx->module, type_name);
              if (td && td->kind == AST_DECL_TYPE) {
                  fprintf(out, "rae_fromJson_%.*s_(", (int)type_name.len, type_name.data);
                  if (!emit_expr(ctx, expr->as.method_call.args->value, out, PREC_LOWEST, false)) return false;
                  fprintf(out, ")");
                  return true;
              }
          }
      }

      if (!func_decl && str_eq_cstr(method, "toBinary") && explicit_args_count == 0) {
          Str type = get_base_type_name_str(infer_expr_type(ctx, expr->as.method_call.object));
          const AstDecl* td = find_type_decl(ctx->module, type);
          if (td && td->kind == AST_DECL_TYPE) {
              fprintf(out, "rae_toBinary_%.*s_(", (int)type.len, type.data);
              bool is_ptr = false;
              if (expr->as.method_call.object->kind == AST_EXPR_IDENT) {
                  is_ptr = is_pointer_type(ctx, expr->as.method_call.object->as.ident);
              }
              if (is_ptr) {
                  if (!emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, false)) return false;
              } else {
                  fprintf(out, "&(");
                  if (!emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, false)) return false;
                  fprintf(out, ")");
              }
              fprintf(out, ", NULL)"); // TODO: support out_size
              return true;
          }
      }

      if (!func_decl && str_eq_cstr(method, "fromBinary") && explicit_args_count == 1) {
          if (expr->as.method_call.object->kind == AST_EXPR_IDENT) {
              Str type_name = expr->as.method_call.object->as.ident;
              const AstDecl* td = find_type_decl(ctx->module, type_name);
              if (td && td->kind == AST_DECL_TYPE) {
                  fprintf(out, "rae_fromBinary_%.*s_(", (int)type_name.len, type_name.data);
                  if (!emit_expr(ctx, expr->as.method_call.args->value, out, PREC_LOWEST, false)) return false;
                  fprintf(out, ", 0)"); // TODO: support size
                  return true;
              }
          }
      }

      if (func_decl) {
        const char* cast_pre = "";
        const char* cast_post = "";
        if (func_decl->returns) {
            Str rtype = d_get_return_type_name(func_decl);
            bool is_generic_ret = false;
            const AstIdentifierPart* gp = func_decl->generic_params;
            while (gp) {
                if (str_eq(gp->text, rtype)) { is_generic_ret = true; break; }
                gp = gp->next;
            }
            if (is_generic_ret) {
                Str inferred = infer_expr_type(ctx, expr);
                
                bool is_opt_ret = false;
                if (func_decl && func_decl->returns && func_decl->returns->type && func_decl->returns->type->is_opt) {
                    is_opt_ret = true;
                }

                if (is_opt_ret) {
                    cast_pre = "((RaeAny)(";
                    cast_post = "))";
                } else {
                    // Fallback: if we are in a match or something where infer_expr_type might fail to specialize,
                    // try to look at receiver
                    if (str_eq_cstr(inferred, "Any")) {
                        const AstExpr* receiver_inner = expr->as.method_call.object;
                        while (receiver_inner->kind == AST_EXPR_UNARY && (receiver_inner->as.unary.op == AST_UNARY_VIEW || receiver_inner->as.unary.op == AST_UNARY_MOD)) {
                            receiver_inner = receiver_inner->as.unary.operand;
                        }
                        if (receiver_inner->kind == AST_EXPR_IDENT) {
                            const AstTypeRef* tr = get_local_type_ref(ctx, receiver_inner->as.ident);
                            if (tr && tr->generic_args && tr->generic_args->parts) {
                                inferred = tr->generic_args->parts->text;
                            }
                        } else if (receiver_inner->kind == AST_EXPR_MEMBER) {
                            Str obj_type = infer_expr_type(ctx, receiver_inner->as.member.object);
                            const AstDecl* d = find_type_decl(ctx->module, obj_type);
                            if (d && d->kind == AST_DECL_TYPE) {
                                for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
                                    if (str_eq(f->name, receiver_inner->as.member.member)) {
                                        if (f->type && f->type->generic_args && f->type->generic_args->parts) {
                                            inferred = f->type->generic_args->parts->text;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (str_eq_cstr(inferred, "Int")) {
                        cast_pre = "((int64_t)(";
                        cast_post = ").as.i)";
                    } else if (str_eq_cstr(inferred, "Float")) {
                        cast_pre = "((double)(";
                        cast_post = ").as.f)";
                    } else if (str_eq_cstr(inferred, "Bool")) {
                        cast_pre = "((int8_t)(";
                        cast_post = ").as.b)";
                    } else if (str_eq_cstr(inferred, "String")) {
                        cast_pre = "((const char*)(";
                        cast_post = ").as.s)";
                    } else {
                        // Custom type / struct pointer
                        bool is_generic_param_name = false;
                        if (ctx && ctx->generic_params) {
                            const AstIdentifierPart* gp = ctx->generic_params;
                            while (gp) {
                                if (str_eq(gp->text, inferred)) { is_generic_param_name = true; break; }
                                gp = gp->next;
                            }
                        }

                        if (str_eq_cstr(inferred, "Any") || is_generic_param_name) {
                            cast_pre = "((RaeAny)(";
                            cast_post = "))";
                        } else {
                            char* type_name = str_to_cstr(inferred);
                            char* buf = malloc(strlen(type_name) + 32);
                            sprintf(buf, "((%s*)(", type_name);
                            cast_pre = buf;
                            cast_post = ").as.ptr)";
                            free(type_name);
                        }
                    }
                }
            }
        }

        fprintf(out, "%s", cast_pre);
        
        emit_mangled_function_name(func_decl, out);
        
        if (fprintf(out, "(") < 0) return false;
        
        const AstExpr* receiver = expr->as.method_call.object;
        bool needs_addr = false;
        if (func_decl->params && (func_decl->params->type->is_view || func_decl->params->type->is_mod)) {
            needs_addr = true;
        }
        
        bool have_ptr = false;
        if (receiver->kind == AST_EXPR_IDENT && is_pointer_type(ctx, receiver->as.ident)) {
            have_ptr = true;
        } else if (receiver->kind == AST_EXPR_UNARY && (receiver->as.unary.op == AST_UNARY_VIEW || receiver->as.unary.op == AST_UNARY_MOD)) {
            have_ptr = true;
        }

        if (needs_addr && !have_ptr) {
             if (func_decl->params && func_decl->params->type && func_decl->params->type->generic_args) {
                 char* cast_type = mangled_type_name_to_cstr(func_decl->params->type);
                 fprintf(out, "((%s*)", cast_type);
                 free(cast_type);
             }
             fprintf(out, "&(");
        } else if (!needs_addr && have_ptr) {
             fprintf(out, "*(");
        }
        
        if (!emit_expr(ctx, receiver, out, PREC_LOWEST, false)) return false;
        
        if (needs_addr && !have_ptr) {
             fprintf(out, ")");
             if (func_decl->params && func_decl->params->type && func_decl->params->type->generic_args) fprintf(out, ")");
        } else if (!needs_addr && have_ptr) {
             fprintf(out, ")");
        }

        const AstCallArg* arg = expr->as.method_call.args;
        const AstParam* param = func_decl->params ? func_decl->params->next : NULL;
        
        while (arg) {
          fprintf(out, ", ");
          
          bool is_any_param = false;
          if (param && param->type && param->type->parts && str_eq_cstr(param->type->parts->text, "Any")) {
              is_any_param = true;
          } else if (param && func_decl->generic_params) {
              const AstIdentifierPart* gp = func_decl->generic_params;
              while (gp) {
                  if (param->type && param->type->parts && str_eq(gp->text, param->type->parts->text)) {
                      is_any_param = true;
                      break;
                  }
                  gp = gp->next;
              }
          }

          if (is_any_param) fprintf(out, "rae_any(");
          else if (param && param->type && param->type->generic_args) {
              char* cast_type = mangled_type_name_to_cstr(param->type);
              fprintf(out, "((%s)(", cast_type); // Note: value cast for Any or similar? 
              // Wait, if it's mod/view it would be a ptr. 
              // Standard add(T) takes T by value.
              free(cast_type);
          }

          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST, false)) return false;

          if (is_any_param) fprintf(out, ")");
          else if (param && param->type && param->type->generic_args) fprintf(out, "))");
          
          arg = arg->next;
          if (param) param = param->next;
        }
        fprintf(out, ")");
        fprintf(out, "%s", cast_post);
        return true;
      }
      
      // Fallback for built-ins or unknown methods
      if (str_eq_cstr(method, "toString")) {
          fprintf(out, "rae_ext_rae_str(");
          if (!emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, false)) return false;
          fprintf(out, ")");
          return true;
      }
      
      return false;
    }
    case AST_EXPR_COLLECTION_LITERAL: {
      uint16_t element_count = 0;
      const AstCollectionElement* current = expr->as.collection.elements;
      while (current) { element_count++; current = current->next; }
      
      bool is_object = (expr->as.collection.elements && expr->as.collection.elements->key != NULL);
      
      if (is_object) {
        if (expr->as.collection.type) {
          fprintf(out, "(");
          if (!emit_type_ref_as_c_type(ctx, expr->as.collection.type, out)) return false;
          fprintf(out, ")");
        }
        fprintf(out, "{ ");
        current = expr->as.collection.elements;
        while (current) {
          if (current->key) {
            fprintf(out, ".%.*s = ", (int)current->key->len, current->key->data);
          }
          if (!emit_expr(ctx, current->value, out, PREC_LOWEST, false)) return false;
          if (current->next) fprintf(out, ", ");
          current = current->next;
        }
        fprintf(out, " }");
        return true;
      }
      
          Str int_type = str_from_cstr("Int");
          Str add_types[] = { str_from_cstr("List"), str_from_cstr("Any") };
          const AstFuncDecl* create_fn = find_function_overload(ctx->module, ctx, str_from_cstr("createList"), &int_type, 1);
          const AstFuncDecl* add_fn = find_function_overload(ctx->module, ctx, str_from_cstr("add"), add_types, 2);
      fprintf(out, "__extension__ ({ ");
      
      fprintf(out, "rae_List_Any_ _l = ");
      
      if (create_fn) emit_mangled_function_name(create_fn, out);
      else fprintf(out, "rae_createList_rae_Int_");
      fprintf(out, "(%u); ", element_count);

      current = expr->as.collection.elements;
      while (current) {
        if (add_fn) {
            emit_mangled_function_name(add_fn, out);
        } else {
            fprintf(out, "rae_add_rae_List_Any__rae_T_");
        }
        // Cast _l to rae_List_Any_* for add
        fprintf(out, "((rae_List_Any_*)&_l, rae_any(");
        if (!emit_expr(ctx, current->value, out, PREC_LOWEST, false)) return false;
        fprintf(out, ")); ");
        current = current->next;
      }
      
      if (ctx->expected_type && ctx->expected_type->generic_args) {
          char* target_str = mangled_type_name_to_cstr(ctx->expected_type);
          fprintf(out, "((union { rae_List_Any_ src; %s dst; }){ .src = _l }).dst; })", target_str);
          free(target_str);
      } else {
          fprintf(out, "_l; })");
      }
      return true;
    }
  }
  fprintf(stderr, "error: unsupported expression kind in C backend\n");
  return false;
}

static bool emit_call(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
  if (expr->kind == AST_EXPR_METHOD_CALL) {
      if (fprintf(out, "  ") < 0) return false;
      if (!emit_expr(ctx, expr, out, PREC_LOWEST, false)) return false;
      return fprintf(out, ";\n") >= 0;
  }
  
  if (expr->kind == AST_EXPR_CALL && expr->as.call.callee && 
      expr->as.call.callee->kind == AST_EXPR_IDENT) {
      Str name = expr->as.call.callee->as.ident;
      if (str_eq_cstr(name, "log")) {
        return emit_log_call(ctx, expr, out, true);
      }
      if (str_eq_cstr(name, "logS")) {
        return emit_log_call(ctx, expr, out, false);
      }
  }

  if (fprintf(out, "  ") < 0) return false;
  if (!emit_expr(ctx, expr, out, PREC_LOWEST, false)) {
    return false;
  }
  if (fprintf(out, ";\n") < 0) {
    return false;
  }
  return true;
}

static bool emit_defers(CFuncContext* ctx, int min_depth, FILE* out) {
  for (int i = (int)ctx->defer_stack.count - 1; i >= 0; i--) {
    if (ctx->defer_stack.entries[i].scope_depth >= min_depth) {
      const AstStmt* stmt = ctx->defer_stack.entries[i].block->first;
      while (stmt) {
        if (!emit_stmt(ctx, stmt, out)) return false;
        stmt = stmt->next;
      }
    }
  }

  return true;
}

static void pop_defers(CFuncContext* ctx, int depth) {
  while (ctx->defer_stack.count > 0 && 
         ctx->defer_stack.entries[ctx->defer_stack.count - 1].scope_depth >= depth) {
    ctx->defer_stack.count--;
  }
}

static bool emit_block(CFuncContext* ctx, const AstBlock* block, FILE* out) {
  if (!block) return true;
  ctx->scope_depth++;
  const AstStmt* inner = block->first;
  while (inner) {
    if (!emit_stmt(ctx, inner, out)) return false;
    inner = inner->next;
  }
  if (!emit_defers(ctx, ctx->scope_depth, out)) return false;
  pop_defers(ctx, ctx->scope_depth);
  ctx->scope_depth--;
  return true;
}

static bool emit_spawn_wrapper(CFuncContext* ctx, const AstFuncDecl* func, FILE* out) {
  fprintf(out, "typedef struct {\n");
  const AstParam* p = func->params;
  while (p) {
    fprintf(out, "  ");
    if (!emit_type_ref_as_c_type(ctx, p->type, out)) return false;
    fprintf(out, " %.*s;\n", (int)p->name.len, p->name.data);
    p = p->next;
  }
  fprintf(out, "} _spawn_args_");
  emit_mangled_function_name(func, out);
  fprintf(out, ";\n\n");

  fprintf(out, "static void* _spawn_wrapper_");
  emit_mangled_function_name(func, out);
  fprintf(out, "(void* data) {\n");
  fprintf(out, "  _spawn_args_");
  emit_mangled_function_name(func, out);
  fprintf(out, "* args = (_spawn_args_");
  emit_mangled_function_name(func, out);
  fprintf(out, "*)data;\n");
  
  fprintf(out, "  ");
  emit_mangled_function_name(func, out);
  fprintf(out, "(");
  p = func->params;
  while (p) {
    fprintf(out, "args->%.*s", (int)p->name.len, p->name.data);
    p = p->next;
    if (p) fprintf(out, ", ");
  }
  fprintf(out, ");\n");
  fprintf(out, "  free(args);\n");
  fprintf(out, "  return NULL;\n");
  fprintf(out, "}\n\n");
  return true;
}

static bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  if (!stmt) return false;
  switch (stmt->kind) {
    case AST_STMT_DEFER: {
      if (ctx->defer_stack.count >= 64) {
        fprintf(stderr, "error: C backend defer stack overflow\n");
        return false;
      }
      ctx->defer_stack.entries[ctx->defer_stack.count].block = stmt->as.defer_stmt.block;
      ctx->defer_stack.entries[ctx->defer_stack.count].scope_depth = ctx->scope_depth;
      ctx->defer_stack.count++;
      return true;
    }
    case AST_STMT_RET: {
      const AstReturnArg* arg = stmt->as.ret_stmt.values;
      if (!arg) {
        if (ctx->returns_value && !ctx->is_main) {
          fprintf(stderr, "error: return without value in function expecting a value\n");
          return false;
        }
        if (!emit_defers(ctx, 0, out)) return false;
        if (ctx->is_main) {
            return fprintf(out, "  return 0;\n") >= 0;
        }
        return fprintf(out, "  return;\n") >= 0;
      }
      if (arg->next) {
        fprintf(stderr, "error: C backend only supports single return values\n");
        return false;
      }
      if (!ctx->returns_value) {
        fprintf(stderr, "error: return with value in non-returning function\n");
        return false;
      }
      
      if (fprintf(out, "  %s _ret = ", ctx->return_type_name) < 0) {
        return false;
      }
      
      bool ret_is_any = false;
      if (ctx->func_decl && ctx->func_decl->returns && 
          ctx->func_decl->returns->type && ctx->func_decl->returns->type->parts) {
          Str rtype = ctx->func_decl->returns->type->parts->text;
          if (str_eq_cstr(rtype, "Any")) {
              ret_is_any = true;
          } else if (ctx->func_decl->generic_params) {
              const AstIdentifierPart* gp = ctx->func_decl->generic_params;
              while (gp) {
                  if (str_eq(gp->text, rtype)) {
                      ret_is_any = true;
                      break;
                  }
                  gp = gp->next;
              }
          }
      }

      if (ret_is_any) {
          if (arg->value->kind == AST_EXPR_NONE) {
              fprintf(out, "rae_any_none();\n");
              goto after_ret_val;
          }
          fprintf(out, "rae_any(");
      }

      // If returning object literal or keyed collection literal without type, add cast
      bool is_obj_like = (arg->value->kind == AST_EXPR_OBJECT && !arg->value->as.object_literal.type) ||
                         (arg->value->kind == AST_EXPR_COLLECTION_LITERAL && !arg->value->as.collection.type && 
                          arg->value->as.collection.elements && arg->value->as.collection.elements->key);
      
      if (is_obj_like) {
          fprintf(out, "(%s)", ctx->return_type_name);
      }
      
      if (!emit_expr(ctx, arg->value, out, PREC_LOWEST, false)) {
        return false;
      }
      
      if (ret_is_any) fprintf(out, ")");
      fprintf(out, ";\n");

after_ret_val:
      if (!emit_defers(ctx, 0, out)) return false;
      return fprintf(out, "  return _ret;\n") >= 0;
    }
        case AST_STMT_LET: {
          if (ctx->local_count >= sizeof(ctx->locals) / sizeof(ctx->locals[0])) {
            fprintf(stderr, "error: C backend local limit exceeded\n");
            return false;
          }
    
          fprintf(out, "  ");
          if (stmt->as.let_stmt.type) {
              bool is_already_const = false;
              if (stmt->as.let_stmt.type->parts) {
                  const char* mapped = map_rae_type_to_c(stmt->as.let_stmt.type->parts->text);
                  if (mapped && strcmp(mapped, "const char*") == 0) is_already_const = true;
              }
              if (stmt->as.let_stmt.type->is_view && !is_already_const) fprintf(out, "const ");
              if (!emit_type_ref_as_c_type(ctx, stmt->as.let_stmt.type, out)) return false;
          } else {
              fprintf(out, "int64_t");
          }
          
          fprintf(out, " %.*s", (int)stmt->as.let_stmt.name.len, stmt->as.let_stmt.name.data);

          if (!stmt->as.let_stmt.value) {
              fprintf(out, " = ");
              if (stmt->as.let_stmt.type) {
                  AstExpr synthetic = {0};
                  synthetic.kind = AST_EXPR_OBJECT;
                  synthetic.as.object_literal.type = stmt->as.let_stmt.type;
                  synthetic.line = stmt->line;
                  synthetic.column = stmt->column;
                  
                  ctx->expected_type = stmt->as.let_stmt.type;
                  if (!emit_expr(ctx, &synthetic, out, PREC_LOWEST, false)) {
                      ctx->expected_type = NULL;
                      return false;
                  }
                  ctx->expected_type = NULL;
              } else {
                  fprintf(out, "{0}");
              }
          } else {
              fprintf(out, " = ");
              bool is_any = (stmt->as.let_stmt.type && stmt->as.let_stmt.type->parts && str_eq_cstr(stmt->as.let_stmt.type->parts->text, "Any"));
              bool is_opt = (stmt->as.let_stmt.type && stmt->as.let_stmt.type->is_opt);
              if (is_any || is_opt) {
                  if (stmt->as.let_stmt.value->kind == AST_EXPR_NONE) {
                      fprintf(out, "rae_any_none()");
                      goto skip_expr;
                  }
                  fprintf(out, "rae_any(");
              }
              
              bool val_needs_addr = stmt->as.let_stmt.is_bind;
              if (val_needs_addr) {
                  if (stmt->as.let_stmt.value->kind == AST_EXPR_CALL || stmt->as.let_stmt.value->kind == AST_EXPR_METHOD_CALL ||
                      stmt->as.let_stmt.value->kind == AST_EXPR_NONE) {
                      val_needs_addr = false;
                  } else if (stmt->as.let_stmt.value->kind == AST_EXPR_IDENT) {
                      if (is_pointer_type(ctx, stmt->as.let_stmt.value->as.ident)) {
                          val_needs_addr = false;
                      }
                  }
              }
              if (val_needs_addr) {
                  if (fprintf(out, "&(") < 0) return false;
              }

              ctx->expected_type = stmt->as.let_stmt.type;

              // Fix: If it's an object literal being assigned, we need a C cast (Type){...}
              if (stmt->as.let_stmt.value->kind == AST_EXPR_OBJECT && !stmt->as.let_stmt.value->as.object_literal.type) {
                  if (stmt->as.let_stmt.type) {
                      fprintf(out, "(");
                      if (!emit_type_ref_as_c_type(ctx, stmt->as.let_stmt.type, out)) return false;
                      fprintf(out, ")");
                  }
              }

              if (!emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false)) {
                ctx->expected_type = NULL;
                return false;
              }
              ctx->expected_type = NULL;
              if (val_needs_addr) {
                  if (fprintf(out, ")") < 0) return false;
              }
              if (is_any || is_opt) {
                  fprintf(out, ")");
              }
skip_expr:;
          }
          
          if (fprintf(out, ";\n") < 0) return false;

          Str base_type_name = str_from_cstr("Int");
          bool is_ptr = false;
          bool is_mod = false;
          if (stmt->as.let_stmt.type) {
              is_ptr = stmt->as.let_stmt.type->is_view || stmt->as.let_stmt.type->is_mod;
              is_mod = stmt->as.let_stmt.type->is_mod;
              if (stmt->as.let_stmt.type->is_id) {
                  base_type_name = str_from_cstr("id");
              } else if (stmt->as.let_stmt.type->is_key) {
                  base_type_name = str_from_cstr("key");
              } else if (stmt->as.let_stmt.type->parts) {
                  base_type_name = stmt->as.let_stmt.type->parts->text;
              }
          } else if (stmt->as.let_stmt.value && stmt->as.let_stmt.value->kind == AST_EXPR_UNARY) {
              if (stmt->as.let_stmt.value->as.unary.op == AST_UNARY_VIEW || stmt->as.let_stmt.value->as.unary.op == AST_UNARY_MOD) {
                  is_ptr = true;
                  if (stmt->as.let_stmt.value->as.unary.op == AST_UNARY_MOD) is_mod = true;
              }
          }
                    ctx->locals[ctx->local_count] = stmt->as.let_stmt.name;
                    ctx->local_types[ctx->local_count] = base_type_name;
                    ctx->local_type_refs[ctx->local_count] = stmt->as.let_stmt.type;
                    ctx->local_is_ptr[ctx->local_count] = is_ptr;
                    ctx->local_is_mod[ctx->local_count] = is_mod;
                    ctx->local_count++;
          
                    return true;
                  }
              case AST_STMT_EXPR:
      if (!emit_call(ctx, stmt->as.expr_stmt, out)) return false;
      return true;
    case AST_STMT_IF:
      return emit_if(ctx, stmt, out);
    case AST_STMT_LOOP:
      return emit_loop(ctx, stmt, out);
    case AST_STMT_MATCH:
      return emit_match(ctx, stmt, out);
    case AST_STMT_ASSIGN: {
      if (fprintf(out, "  ") < 0) return false;
      
      bool is_ptr = false;
      if (stmt->as.assign_stmt.target->kind == AST_EXPR_IDENT) {
          if (is_pointer_type(ctx, stmt->as.assign_stmt.target->as.ident)) {
              is_ptr = true;
          }
      }
      
      if (is_ptr && !stmt->as.assign_stmt.is_bind) {
          fprintf(out, "(*");
          if (!emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_ASSIGN, true)) return false;
          fprintf(out, ")");
      } else {
          if (!emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_ASSIGN, true)) return false;
      }
      
      
      if (fprintf(out, " = ") < 0) return false;
      
      bool is_any = false;
      bool is_opt = false;
      if (stmt->as.assign_stmt.target->kind == AST_EXPR_IDENT) {
          const AstTypeRef* type = get_local_type_ref(ctx, stmt->as.assign_stmt.target->as.ident);
          if (type && type->parts && str_eq_cstr(type->parts->text, "Any")) is_any = true;
          if (type && type->is_opt) is_opt = true;
      }
      
      if (is_any || is_opt) {
          if (stmt->as.assign_stmt.value->kind == AST_EXPR_NONE) {
              fprintf(out, "rae_any_none()");
              goto skip_assign_expr;
          }
          fprintf(out, "rae_any(");
      }

      bool val_needs_addr = stmt->as.assign_stmt.is_bind;
      if (val_needs_addr) {
          if (stmt->as.assign_stmt.value->kind == AST_EXPR_CALL || stmt->as.assign_stmt.value->kind == AST_EXPR_METHOD_CALL ||
              stmt->as.assign_stmt.value->kind == AST_EXPR_NONE) {
              val_needs_addr = false;
          } else if (stmt->as.assign_stmt.value->kind == AST_EXPR_IDENT) {
              if (is_pointer_type(ctx, stmt->as.assign_stmt.value->as.ident)) {
                  val_needs_addr = false;
              }
          }
      }
      if (val_needs_addr) {
          if (fprintf(out, "&(") < 0) return false;
      }

      if (stmt->as.assign_stmt.target->kind == AST_EXPR_IDENT) {
          const AstTypeRef* tr = get_local_type_ref(ctx, stmt->as.assign_stmt.target->as.ident);
          if (tr && is_ptr && !stmt->as.assign_stmt.is_bind) {
              AstTypeRef base_tr = *tr;
              base_tr.is_view = false;
              base_tr.is_mod = false;
              ctx->expected_type = &base_tr;
              if (!emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false)) {
                  ctx->expected_type = NULL;
                  return false;
              }
              ctx->expected_type = NULL;
          } else {
              ctx->expected_type = tr;
              if (!emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false)) {
                  ctx->expected_type = NULL;
                  return false;
              }
              ctx->expected_type = NULL;
          }
      } else {
          if (!emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false)) {
              return false;
          }
      }
      if (val_needs_addr) {
          if (fprintf(out, ")") < 0) return false;
      }
      if (is_any || is_opt) fprintf(out, ")");
skip_assign_expr:
      if (fprintf(out, ";\n") < 0) return false;
      return true;
    }
    default:
      fprintf(stderr, "error: C backend does not yet support this statement kind (%d)\n",
              (int)stmt->kind);
      return false;
  }
}

typedef struct {
    const char* rae_name;
    const char* c_name;
} NativeMap;

static const NativeMap RAYLIB_MAP[] = {
    {"initWindow", "InitWindow"},
    {"windowShouldClose", "WindowShouldClose"},
    {"closeWindow", "CloseWindow"},
    {"beginDrawing", "BeginDrawing"},
    {"endDrawing", "EndDrawing"},
    {"setTargetFPS", "SetTargetFPS"},
    {"getScreenWidth", "GetScreenWidth"},
    {"getScreenHeight", "GetScreenHeight"},
    {"isKeyDown", "IsKeyDown"},
    {"isKeyPressed", "IsKeyPressed"},
    {"clearBackground", "ClearBackground"},
    {"loadTexture", "LoadTexture"},
    {"unloadTexture", "UnloadTexture"},
    {"drawTexture", "DrawTexture"},
    {"drawTextureEx", "DrawTextureEx"},
    {"drawRectangle", "DrawRectangle"},
    {"drawRectangleLines", "DrawRectangleLines"},
    {"drawCircle", "DrawCircle"},
    {"drawText", "DrawText"},
    {"drawCube", "DrawCube"},
    {"drawSphere", "DrawSphere"},
    {"drawCylinder", "DrawCylinder"},
    {"drawGrid", "DrawGrid"},
    {"beginMode3D", "BeginMode3D"},
    {"endMode3D", "EndMode3D"},
    {NULL, NULL}
};

static const char* find_raylib_mapping(Str name) {
    for (int i = 0; RAYLIB_MAP[i].rae_name; i++) {
        if (str_eq_cstr(name, RAYLIB_MAP[i].rae_name)) return RAYLIB_MAP[i].c_name;
    }
    return NULL;
}

static bool emit_json_methods(CFuncContext* ctx, FILE* out, bool uses_raylib) {
  (void)ctx;
  (void)uses_raylib;
  
  // 1. Forward declarations
  for (size_t i = 0; i < g_all_decl_count; i++) {
    const AstDecl* decl = g_all_decls[i];
    if (decl->kind == AST_DECL_TYPE) {
      const AstTypeDecl* td = &decl->as.type_decl;
      if (td->generic_params) continue; // Skip generic templates
      if (has_property(td->properties, "c_struct")) continue;
      if (is_raylib_builtin_type(td->name)) continue;
      fprintf(out, "RAE_UNUSED static const char* rae_toJson_%.*s_(%.*s* this);\n",
              (int)td->name.len, td->name.data, (int)td->name.len, td->name.data);
      fprintf(out, "RAE_UNUSED static %.*s rae_fromJson_%.*s_(const char* json);\n",
              (int)td->name.len, td->name.data, (int)td->name.len, td->name.data);
      fprintf(out, "RAE_UNUSED static void* rae_toBinary_%.*s_(%.*s* this, int64_t* out_size);\n",
              (int)td->name.len, td->name.data, (int)td->name.len, td->name.data);
      fprintf(out, "RAE_UNUSED static %.*s rae_fromBinary_%.*s_(void* data, int64_t size);\n",
              (int)td->name.len, td->name.data, (int)td->name.len, td->name.data);
    }
  }
  fprintf(out, "\n");

  // 2. Implementations
  for (size_t i = 0; i < g_all_decl_count; i++) {
    const AstDecl* decl = g_all_decls[i];
    if (decl->kind == AST_DECL_TYPE) {
      const AstTypeDecl* td = &decl->as.type_decl;
      if (td->generic_params) continue; // Skip generic templates
      if (has_property(td->properties, "c_struct")) continue;
      if (is_raylib_builtin_type(td->name)) continue;
      
      // toJson
      fprintf(out, "RAE_UNUSED static const char* rae_toJson_%.*s_(%.*s* this) {\n",
              (int)td->name.len, td->name.data, (int)td->name.len, td->name.data);
      fprintf(out, "  const char* res = \"{\";\n");
      
      const AstTypeField* f = td->fields;
      while (f) {
        fprintf(out, "  res = rae_ext_rae_str_concat(res, \"\\\"%.*s\\\": \");\n", (int)f->name.len, f->name.data);
        Str base = get_base_type_name(f->type);
        if (str_eq_cstr(base, "Int")) {
          fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->%.*s));\n", (int)f->name.len, f->name.data);
        } else if (str_eq_cstr(base, "Float")) {
          fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_f64(this->%.*s));\n", (int)f->name.len, f->name.data);
        } else if (str_eq_cstr(base, "Bool")) {
          fprintf(out, "  res = rae_ext_rae_str_concat(res, this->%.*s ? \"true\" : \"false\");\n", (int)f->name.len, f->name.data);
        } else if (str_eq_cstr(base, "String")) {
          fprintf(out, "  res = rae_ext_rae_str_concat(res, \"\\\"\");\n");
          fprintf(out, "  res = rae_ext_rae_str_concat(res, this->%.*s ? this->%.*s : \"\");\n", (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
          fprintf(out, "  res = rae_ext_rae_str_concat(res, \"\\\"\");\n");
        } else if (find_type_decl(ctx->module, base)) {
          if (is_raylib_builtin_type(base)) {
              if (str_eq_cstr(base, "Vector2")) {
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, \"{\\\"x\\\": \");\n");
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_f64(this->%.*s.x));\n", (int)f->name.len, f->name.data);
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, \", \\\"y\\\": \");\n");
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_f64(this->%.*s.y));\n", (int)f->name.len, f->name.data);
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, \"}\");\n");
              } else if (str_eq_cstr(base, "Vector3")) {
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, \"{\\\"x\\\": \");\n");
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_f64(this->%.*s.x));\n", (int)f->name.len, f->name.data);
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, \", \\\"y\\\": \");\n");
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_f64(this->%.*s.y));\n", (int)f->name.len, f->name.data);
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, \", \\\"z\\\": \");\n");
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_f64(this->%.*s.z));\n", (int)f->name.len, f->name.data);
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, \"}\");\n");
              } else if (str_eq_cstr(base, "Color")) {
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, \"{\\\"r\\\": \");\n");
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->%.*s.r));\n", (int)f->name.len, f->name.data);
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, \", \\\"g\\\": \");\n");
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->%.*s.g));\n", (int)f->name.len, f->name.data);
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, \", \\\"b\\\": \");\n");
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->%.*s.b));\n", (int)f->name.len, f->name.data);
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, \", \\\"a\\\": \");\n");
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->%.*s.a));\n", (int)f->name.len, f->name.data);
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, \"}\");\n");
              } else {
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, \"null\");\n");
              }
          } else {
              // Check if the type is actually a generic parameter of td
              bool is_generic_param = false;
              const AstIdentifierPart* gp = td->generic_params;
              while (gp) {
                  if (str_eq(gp->text, base)) { is_generic_param = true; break; }
                  gp = gp->next;
              }

              if (is_generic_param || f->type->is_opt) {
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_str_any(this->%.*s));\n", (int)f->name.len, f->name.data);
              } else {
                  // Check if the type has a toJson method
                  fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_toJson_%.*s_(&this->%.*s));\n", (int)base.len, base.data, (int)f->name.len, f->name.data);
              }
          }
        } else if (find_enum_decl(ctx->module, base)) {
          fprintf(out, "  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->%.*s));\n", (int)f->name.len, f->name.data);
        } else {
          fprintf(out, "  res = rae_ext_rae_str_concat(res, \"null\");\n");
        }
        
        f = f->next;
        if (f) fprintf(out, "  res = rae_ext_rae_str_concat(res, \", \");\n");
      }
      
      fprintf(out, "  res = rae_ext_rae_str_concat(res, \"}\");\n");
      fprintf(out, "  return res;\n");
      fprintf(out, "}\n\n");

      // fromJson
      fprintf(out, "RAE_UNUSED static %.*s rae_fromJson_%.*s_(const char* json) {\n",
              (int)td->name.len, td->name.data, (int)td->name.len, td->name.data);
      fprintf(out, "  %.*s res = {0};\n", (int)td->name.len, td->name.data);
      fprintf(out, "  RaeAny val;\n");
      
      f = td->fields;
      while (f) {
        fprintf(out, "  val = rae_ext_json_get(json, \"%.*s\");\n", (int)f->name.len, f->name.data);
        Str base = get_base_type_name(f->type);
        if (str_eq_cstr(base, "Int")) {
          fprintf(out, "  if (val.type == RAE_TYPE_INT) res.%.*s = val.as.i;\n", (int)f->name.len, f->name.data);
        } else if (str_eq_cstr(base, "Float")) {
          fprintf(out, "  if (val.type == RAE_TYPE_FLOAT) res.%.*s = val.as.f;\n", (int)f->name.len, f->name.data);
          fprintf(out, "  else if (val.type == RAE_TYPE_INT) res.%.*s = (double)val.as.i;\n", (int)f->name.len, f->name.data);
        } else if (str_eq_cstr(base, "Bool")) {
          fprintf(out, "  if (val.type == RAE_TYPE_BOOL) res.%.*s = val.as.b;\n", (int)f->name.len, f->name.data);
        } else if (str_eq_cstr(base, "String")) {
          fprintf(out, "  if (val.type == RAE_TYPE_STRING) res.%.*s = val.as.s;\n", (int)f->name.len, f->name.data);
        } else if (find_type_decl(ctx->module, base)) {
          if (is_raylib_builtin_type(base)) {
              fprintf(out, "  if (val.type == RAE_TYPE_STRING) {\n");
              if (str_eq_cstr(base, "Vector2")) {
                  fprintf(out, "    RaeAny field_val;\n");
                  fprintf(out, "    field_val = rae_ext_json_get(val.as.s, \"x\"); if (field_val.type == RAE_TYPE_FLOAT) res.%.*s.x = field_val.as.f; else if (field_val.type == RAE_TYPE_INT) res.%.*s.x = (double)field_val.as.i;\n", (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
                  fprintf(out, "    field_val = rae_ext_json_get(val.as.s, \"y\"); if (field_val.type == RAE_TYPE_FLOAT) res.%.*s.y = field_val.as.f; else if (field_val.type == RAE_TYPE_INT) res.%.*s.y = (double)field_val.as.i;\n", (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
              } else if (str_eq_cstr(base, "Vector3")) {
                  fprintf(out, "    RaeAny field_val;\n");
                  fprintf(out, "    field_val = rae_ext_json_get(val.as.s, \"x\"); if (field_val.type == RAE_TYPE_FLOAT) res.%.*s.x = field_val.as.f; else if (field_val.type == RAE_TYPE_INT) res.%.*s.x = (double)field_val.as.i;\n", (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
                  fprintf(out, "    field_val = rae_ext_json_get(val.as.s, \"y\"); if (field_val.type == RAE_TYPE_FLOAT) res.%.*s.y = field_val.as.f; else if (field_val.type == RAE_TYPE_INT) res.%.*s.y = (double)field_val.as.i;\n", (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
                  fprintf(out, "    field_val = rae_ext_json_get(val.as.s, \"z\"); if (field_val.type == RAE_TYPE_FLOAT) res.%.*s.z = field_val.as.f; else if (field_val.type == RAE_TYPE_INT) res.%.*s.z = (double)field_val.as.i;\n", (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
              } else if (str_eq_cstr(base, "Color")) {
                  fprintf(out, "    RaeAny field_val;\n");
                  fprintf(out, "    field_val = rae_ext_json_get(val.as.s, \"r\"); if (field_val.type == RAE_TYPE_INT) res.%.*s.r = field_val.as.i;\n", (int)f->name.len, f->name.data);
                  fprintf(out, "    field_val = rae_ext_json_get(val.as.s, \"g\"); if (field_val.type == RAE_TYPE_INT) res.%.*s.g = field_val.as.i;\n", (int)f->name.len, f->name.data);
                  fprintf(out, "    field_val = rae_ext_json_get(val.as.s, \"b\"); if (field_val.type == RAE_TYPE_INT) res.%.*s.b = field_val.as.i;\n", (int)f->name.len, f->name.data);
                  fprintf(out, "    field_val = rae_ext_json_get(val.as.s, \"a\"); if (field_val.type == RAE_TYPE_INT) res.%.*s.a = field_val.as.i;\n", (int)f->name.len, f->name.data);
              }
              fprintf(out, "  }\n");
          } else {
              bool is_generic_param = false;
              const AstIdentifierPart* gp = td->generic_params;
              while (gp) {
                  if (str_eq(gp->text, base)) { is_generic_param = true; break; }
                  gp = gp->next;
              }
              if (is_generic_param || f->type->is_opt) {
                  fprintf(out, "  res.%.*s = val;\n", (int)f->name.len, f->name.data);
              } else {
                  fprintf(out, "  if (val.type == RAE_TYPE_STRING) res.%.*s = rae_fromJson_%.*s_(val.as.s);\n", (int)f->name.len, f->name.data, (int)base.len, base.data);
              }
          }
        } else if (find_enum_decl(ctx->module, base)) {
          fprintf(out, "  if (val.type == RAE_TYPE_INT) res.%.*s = val.as.i;\n", (int)f->name.len, f->name.data);
        }
        f = f->next;
      }
      fprintf(out, "  return res;\n");
      fprintf(out, "}\n\n");

      // toBinary (Stubs for now)
      fprintf(out, "RAE_UNUSED static void* rae_toBinary_%.*s_(%.*s* this, int64_t* out_size) {\n",
              (int)td->name.len, td->name.data, (int)td->name.len, td->name.data);
      fprintf(out, "  (void)this;\n");
      fprintf(out, "  if (out_size) *out_size = 0;\n");
      fprintf(out, "  return NULL;\n");
      fprintf(out, "}\n\n");

      // fromBinary (Stubs for now)
      fprintf(out, "RAE_UNUSED static %.*s rae_fromBinary_%.*s_(void* data, int64_t size) {\n",
              (int)td->name.len, td->name.data, (int)td->name.len, td->name.data);
      fprintf(out, "  (void)data; (void)size;\n");
      fprintf(out, "  %.*s res = {0};\n", (int)td->name.len, td->name.data);
      fprintf(out, "  return res;\n");
      fprintf(out, "}\n\n");
    }
  }

  // 3. Specialized generic JSON methods
  for (size_t i = 0; i < g_emitted_generic_type_count; i++) {
    fprintf(out, "RAE_UNUSED static const char* rae_toJson_");
    emit_mangled_type_name(g_emitted_generic_types[i], out);
    fprintf(out, "_(void* this) {\n");
    fprintf(out, "  (void)this;\n");
    fprintf(out, "  return \"{}\";\n");
    fprintf(out, "}\n\n");
  }

  return true;
}

static bool emit_raylib_wrapper(const AstFuncDecl* fn, const char* c_name, FILE* out, const AstModule* module) {
  CFuncContext temp_ctx = {.module = module, .generic_params = fn->generic_params, .uses_raylib = true};
  const char* return_type = c_return_type(&temp_ctx, fn);
  if (!return_type) return false;
  
  // If it's one of the ones we put in rae_runtime.c, don't emit implementation here
  if (str_eq_cstr(fn->name, "initWindow") || 
      str_eq_cstr(fn->name, "setConfigFlags") ||
      str_eq_cstr(fn->name, "drawCubeWires") ||
      str_eq_cstr(fn->name, "drawSphere") ||
      str_eq_cstr(fn->name, "getTime") ||
      str_eq_cstr(fn->name, "colorFromHSV")) {
      return true;
  }

  bool is_ext = fn->is_extern || str_starts_with_cstr(fn->name, "rae_ext_");
  if (is_ext) {
      fprintf(out, "RAE_UNUSED %s ", return_type);
  } else {
      fprintf(out, "RAE_UNUSED static %s ", return_type);
  }
  
  emit_mangled_function_name(fn, out);
  fprintf(out, "(");
  if (!emit_param_list(&temp_ctx, fn->params, out, false)) return false;
  fprintf(out, ") {\n");
    
  if (strcmp(return_type, "void") != 0) fprintf(out, "  return ");
  else fprintf(out, "  ");
  
  fprintf(out, "%s(", c_name);
  const AstParam* p = fn->params;
  while (p) {
      Str type_name = get_base_type_name(p->type);
      const AstDecl* td = find_type_decl(module, type_name);
      
      bool is_mod = p->type && p->type->is_mod;
      bool is_explicit_view = p->type && p->type->is_view;
      bool is_val = p->type && p->type->is_val;
      bool is_primitive = is_primitive_type(type_name) || is_raylib_builtin_type(type_name);
      bool is_ptr = (is_mod || is_explicit_view || (!is_val && !is_primitive));
      const char* op = is_ptr ? "->" : ".";

      if (td && td->kind == AST_DECL_TYPE && has_property(td->as.type_decl.properties, "c_struct")) {
          if (str_eq_cstr(type_name, "Color")) {
              fprintf(out, "(Color){ (unsigned char)%.*s%sr, (unsigned char)%.*s%sg, (unsigned char)%.*s%sb, (unsigned char)%.*s%sa }", 
                      (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op);
          } else if (str_eq_cstr(type_name, "Vector2")) {
              fprintf(out, "(Vector2){ (float)%.*s%sx, (float)%.*s%sy }", 
                      (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op);
          } else if (str_eq_cstr(type_name, "Vector3")) {
              fprintf(out, "(Vector3){ (float)%.*s%sx, (float)%.*s%sy, (float)%.*s%sz }", 
                      (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op);
          } else if (str_eq_cstr(type_name, "Texture")) {
              fprintf(out, "(Texture){ .id = (unsigned int)%.*s%sid, .width = (int)%.*s%swidth, .height = (int)%.*s%sheight, .mipmaps = (int)%.*s%smipmaps, .format = (int)%.*s%sformat }", 
                      (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op);
          } else if (str_eq_cstr(type_name, "Camera3D")) {
              fprintf(out, "(Camera3D){ ");
              fprintf(out, ".position = (Vector3){ (float)%.*s%sposition.x, (float)%.*s%sposition.y, (float)%.*s%sposition.z }, ", 
                      (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op);
              fprintf(out, ".target = (Vector3){ (float)%.*s%starget.x, (float)%.*s%starget.y, (float)%.*s%starget.z }, ", 
                      (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op);
              fprintf(out, ".up = (Vector3){ (float)%.*s%sup.x, (float)%.*s%sup.y, (float)%.*s%sup.z }, ", 
                      (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op);
              fprintf(out, ".fovy = (float)%.*s%sfovy, .projection = (int)%.*s%sprojection ", 
                      (int)p->name.len, p->name.data, op, (int)p->name.len, p->name.data, op);
              fprintf(out, "}");
          } else {
              fprintf(out, "%.*s", (int)p->name.len, p->name.data);
          }
      } else if (str_eq_cstr(type_name, "Int")) {
          fprintf(out, "(int)%.*s", (int)p->name.len, p->name.data);
      } else if (str_eq_cstr(type_name, "Float")) {
          if (str_eq_cstr(fn->name, "drawRectangleLines")) {
              fprintf(out, "(int)%.*s", (int)p->name.len, p->name.data);
          } else {
              fprintf(out, "(float)%.*s", (int)p->name.len, p->name.data);
          }
      } else {
          fprintf(out, "%.*s", (int)p->name.len, p->name.data);
      }
      
      p = p->next;
      if (p) fprintf(out, ", ");
  }
  fprintf(out, ");\n");
  fprintf(out, "}\n\n");
  return true;
}

static bool emit_function(const AstModule* module, const AstFuncDecl* func, FILE* out, const struct VmRegistry* registry, bool uses_raylib) {
  if (!func || !func->body) return false;
  if (func->is_extern) {
    return true;
  }
  if (!func->body) {
    fprintf(stderr, "error: C backend requires function bodies to be present\n");
    return false;
  }
  
  const AstIdentifierPart* generic_params = func->generic_params;
  
  bool is_main = str_eq_cstr(func->name, "main");
  
  CFuncContext temp_ctx = {.module = module, .generic_params = generic_params, .registry = registry, .uses_raylib = uses_raylib};
  const char* return_type = c_return_type(&temp_ctx, func);
  if (!return_type) {
    return false;
  }
  bool returns_value = strcmp(return_type, "void") != 0;
  if (func->returns && func->returns->next) {
    fprintf(stderr, "error: C backend only supports single return values per function\n");
    return false;
  }
  
  char* name = str_to_cstr(func->name);
  if (!name) return false;
  
  bool ok = true;
  if (is_main) {
    ok = fprintf(out, "int main(") >= 0;
  } else {
    fprintf(out, "RAE_UNUSED static %s ", return_type);
    emit_mangled_function_name(func, out);
    fprintf(out, "(");
    ok = true;
  }
  
  if (ok) {
    ok = emit_param_list(&temp_ctx, func->params, out, false);
  }
  
  if (ok) {
    ok = fprintf(out, ") {\n") >= 0;
  }
  
  if (!ok) {
    free(name);
    return false;
  }

  CFuncContext ctx = {.module = module,
                      .func_decl = func,
                      .params = func->params,
                      .generic_params = generic_params,
                      .return_type_name = return_type,
                      .local_count = 0,
                      .returns_value = returns_value,
                      .temp_counter = 0,
                      .uses_raylib = uses_raylib,
                      .is_main = is_main,
                      .scope_depth = 0,
                      .defer_stack = {0}};

  // Populate initial locals from parameters
  const AstParam* p = func->params;
  while (p) {
      if (ctx.local_count < 256) {
          ctx.locals[ctx.local_count] = p->name;
          ctx.local_type_refs[ctx.local_count] = p->type;
          ctx.local_types[ctx.local_count] = get_base_type_name(p->type);
          
          bool is_ptr = false;
          bool is_mod = false;
          if (p->type) {
              is_mod = p->type->is_mod;
              if (p->type->is_id) {
                  is_ptr = false;
              } else if (p->type->is_key) {
                  is_ptr = false;
              } else if (p->type->parts) {
                  Str base = p->type->parts->text;
                  bool is_val = p->type->is_val;
                  bool is_explicit_view = p->type->is_view;
                  bool is_primitive = is_primitive_type(base) || (uses_raylib && is_raylib_builtin_type(base));
                  if (!is_primitive && ctx.module && find_enum_decl(ctx.module, base)) {
                      is_primitive = true;
                  }
                  is_ptr = (is_mod || is_explicit_view || (!is_val && !is_primitive));
                  
                  // Apply view-by-default logic to the TypeRef itself if it's not val, mod, or primitive
                  if (!is_val && !is_mod && !is_primitive) {
                      p->type->is_view = true;
                  }
              }
          }
          ctx.local_is_ptr[ctx.local_count] = is_ptr;
          ctx.local_is_mod[ctx.local_count] = is_mod;
          ctx.local_count++;
      }
      p = p->next;
  }
                      
  ctx.local_count = 0; // reset for actual emission if needed, wait no it's already populated
  // Wait, I should probably NOT reset it here if I just populated it.
  // Actually CFuncContext is local to this function.

  if (!emit_block(&ctx, func->body, out)) {
    ok = false;
  }
  
  if (ok) {
    if (!emit_defers(&ctx, 0, out)) ok = false;
  }

  if (ok && is_main) {
    ok = fprintf(out, "  return 0;\n") >= 0;
  }
  
  if (ok) {
    ok = fprintf(out, "}\n\n") >= 0;
  }

  if (ok && !is_main) {
      ok = emit_spawn_wrapper(&ctx, func, out);
  }
  
  free(name);
  return ok;
}

static bool has_property(const AstProperty* props, const char* name) {
  while (props) {
    if (str_eq_cstr(props->name, name)) return true;
    props = props->next;
  }
  return false;
}

static const AstModule* g_find_module_stack[64];
static size_t g_find_module_stack_count = 0;

static const AstDecl* find_type_decl(const AstModule* module, Str name) {
  if (!module) return NULL;
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_TYPE && str_eq(decl->as.type_decl.name, name)) {
      return decl;
    }
  }
  
  for (size_t i = 0; i < g_find_module_stack_count; i++) {
      if (g_find_module_stack[i] == module) return NULL;
  }
  if (g_find_module_stack_count >= 64) return NULL;
  g_find_module_stack[g_find_module_stack_count++] = module;

  const AstDecl* found = NULL;
  for (const AstImport* imp = module->imports; imp; imp = imp->next) {
    found = find_type_decl(imp->module, name);
    if (found) break;
  }
  
  g_find_module_stack_count--;
  return found;
}

static const AstDecl* find_enum_decl(const AstModule* module, Str name) {
  if (!module) return NULL;
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_ENUM && str_eq(decl->as.enum_decl.name, name)) {
      return decl;
    }
  }
  
  for (size_t i = 0; i < g_find_module_stack_count; i++) {
      if (g_find_module_stack[i] == module) return NULL;
  }
  if (g_find_module_stack_count >= 64) return NULL;
  g_find_module_stack[g_find_module_stack_count++] = module;

  const AstDecl* found = NULL;
  for (const AstImport* imp = module->imports; imp; imp = imp->next) {
    found = find_enum_decl(imp->module, name);
    if (found) break;
  }
  
  g_find_module_stack_count--;
  return found;
}

static bool emit_single_struct_def(const AstModule* module, const AstDecl* decl, FILE* out, 
                                   Str* emitted_types, size_t* emitted_count, bool uses_raylib) {
  // Check if already emitted
  for (size_t i = 0; i < *emitted_count; ++i) {
    if (str_eq(emitted_types[i], decl->as.type_decl.name)) return true;
  }

  Str name_str = decl->as.type_decl.name;
  if (is_primitive_type(name_str)) {
      emitted_types[*emitted_count] = name_str;
      (*emitted_count)++;
      return true;
  }
  if (uses_raylib && is_raylib_builtin_type(name_str)) {
      emitted_types[*emitted_count] = name_str;
      (*emitted_count)++;
      return true;
  }

  // Mark as emitted to prevent infinite recursion on cycles (though cycles require pointers in C)
  // We'll add it to the list at the END, but for cycle detection in a robust compiler we'd need a "visiting" set.
  // For now, valid C structs can't have value-cycles.
  
  if (has_property(decl->as.type_decl.properties, "c_struct")) {
      emitted_types[*emitted_count] = decl->as.type_decl.name;
      (*emitted_count)++;
      return true;
  }

  // Emit dependencies first
  const AstTypeField* field = decl->as.type_decl.fields;
  while (field) {
    if (field->type && field->type->parts && !field->type->is_view && !field->type->is_mod) {
      // It's a value field. Check if it's a user type.
      Str type_name = field->type->parts->text;
      bool skip_dep = is_primitive_type(type_name) || (uses_raylib && is_raylib_builtin_type(type_name));
      if (!skip_dep) {
        const AstDecl* dep = find_type_decl(module, type_name);
        if (dep) {
          if (!emit_single_struct_def(module, dep, out, emitted_types, emitted_count, uses_raylib)) return false;
        }
      }
    }
    field = field->next;
  }

  // Now emit this struct
  char* name = str_to_cstr(decl->as.type_decl.name);
  if (fprintf(out, "typedef struct {\n") < 0) { free(name); return false; }
  
  CFuncContext temp_ctx = {.generic_params = decl->as.type_decl.generic_params, .uses_raylib = uses_raylib};

  field = decl->as.type_decl.fields;
  while (field) {
    fprintf(out, "  ");
    bool is_ptr = field->type && (field->type->is_view || field->type->is_mod);
    if (!emit_type_ref_as_c_type(&temp_ctx, field->type, out)) { free(name); return false; }
    if (fprintf(out, "%s %.*s;\n", is_ptr ? "*" : "", (int)field->name.len, field->name.data) < 0) {
      free(name); return false;
    }
    field = field->next;
  }
  if (fprintf(out, "} %s;\n\n", name) < 0) { free(name); return false; }
  free(name);

  emitted_types[*emitted_count] = decl->as.type_decl.name;
  (*emitted_count)++;
  return true;
}

static bool emit_enum_defs(const AstModule* module, FILE* out, bool uses_raylib) {
  (void)module;
  for (size_t i = 0; i < g_all_decl_count; i++) {
    const AstDecl* decl = g_all_decls[i];
    if (decl->kind == AST_DECL_ENUM) {
      if (is_primitive_type(decl->as.enum_decl.name)) continue;
      if (uses_raylib && is_raylib_builtin_type(decl->as.enum_decl.name)) continue;
      fprintf(out, "typedef enum {\n");
      const AstEnumMember* m = decl->as.enum_decl.members;
      while (m) {
        fprintf(out, "  %.*s_%.*s", (int)decl->as.enum_decl.name.len, decl->as.enum_decl.name.data,
                (int)m->name.len, m->name.data);
        if (m->next) fprintf(out, ",");
        fprintf(out, "\n");
        m = m->next;
      }
      fprintf(out, "} %.*s;\n\n", (int)decl->as.enum_decl.name.len, decl->as.enum_decl.name.data);
    }
  }
  return true;
}

static bool emit_struct_defs(const AstModule* module, FILE* out, bool uses_raylib) {
  // Simple array to track emitted types
  Str emitted_types[512]; // increased limit
  size_t emitted_count = 0;

  for (size_t i = 0; i < g_all_decl_count; i++) {
    const AstDecl* decl = g_all_decls[i];
    if (decl->kind == AST_DECL_TYPE) {
      // Don't emit generic templates, only specializations
      if (decl->as.type_decl.generic_params) continue;
      if (!emit_single_struct_def(module, decl, out, emitted_types, &emitted_count, uses_raylib)) return false;
    }
  }

  // 2. Forward declarations for specialized generic structs
  for (size_t i = 0; i < g_generic_type_count; i++) {
    fprintf(out, "typedef struct ");
    emit_mangled_type_name(g_generic_types[i], out);
    fprintf(out, " ");
    emit_mangled_type_name(g_generic_types[i], out);
    fprintf(out, ";\n");
  }
  fprintf(out, "\n");

  // 3. Specialized generic structs
  for (size_t i = 0; i < g_generic_type_count; i++) {
    if (!emit_specialized_struct_def(module, g_generic_types[i], out, uses_raylib)) return false;
  }
  return true;
}

bool c_backend_emit_module(const AstModule* module, const char* out_path, struct VmRegistry* registry, bool* out_uses_raylib) {
  if (!module || !out_path) return false;

  g_all_decl_count = 0;
  g_generic_type_count = 0;
  g_emitted_generic_type_count = 0;
  collect_decls_from_module(module);
  collect_type_refs_module();

  if (out_uses_raylib) *out_uses_raylib = false;
  FILE* out = fopen(out_path, "w");
  if (!out) {
    fprintf(stderr, "error: unable to open '%s' for C backend output\n", out_path);
    return false;
  }

  // Pre-scan for Raylib usage
  bool uses_raylib = false;
  const AstImport* imp = module->imports;
  while (imp) {
      char* imp_path = str_to_cstr(imp->path);
      if (imp_path && strcmp(imp_path, "raylib") == 0) {
          uses_raylib = true;
          free(imp_path);
          break;
      }
      if (imp_path) free(imp_path);
      imp = imp->next;
  }

  if (!uses_raylib) {
      const AstDecl* scan = module->decls;
      while (scan) {
          if (scan->kind == AST_DECL_FUNC) {
              if (find_raylib_mapping(scan->as.func_decl.name) ||
                  str_eq_cstr(scan->as.func_decl.name, "rae_ext_drawCubeWires") ||
                  str_eq_cstr(scan->as.func_decl.name, "rae_ext_drawSphere") ||
                  str_eq_cstr(scan->as.func_decl.name, "rae_ext_initWindow")) {
                  uses_raylib = true;
                  break;
              }
          }
          scan = scan->next;
      }
  }
  
  CFuncContext ctx = {0};
  ctx.module = module;
  ctx.registry = registry;
  ctx.uses_raylib = uses_raylib;
  bool ok = true;
  
  if (uses_raylib) {
      if (out_uses_raylib) *out_uses_raylib = true;
      if (fprintf(out, "#ifndef RAE_HAS_RAYLIB\n") < 0) return false;
      if (fprintf(out, "#define RAE_HAS_RAYLIB\n") < 0) return false;
      if (fprintf(out, "#endif\n") < 0) return false;
      if (fprintf(out, "#include <raylib.h>\n") < 0) return false;
  }
  
  if (fprintf(out, "#include \"rae_runtime.h\"\n\n") < 0) return false;

  if (!emit_enum_defs(module, out, uses_raylib)) {
    fclose(out);
    return false;
  }

  if (!emit_struct_defs(module, out, uses_raylib)) {
    fclose(out);
    return false;
  }

  if (!emit_json_methods(&ctx, out, uses_raylib)) {
    fclose(out);
    return false;
  }


  size_t func_count = 0;
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_FUNC) {
      func_count += 1;
    }
  }
  if (func_count == 0) {
    fprintf(stderr, "error: C backend requires at least one function (expected func main)\n");
    fclose(out);
    return false;
  }
  const AstFuncDecl** funcs = calloc(func_count, sizeof(*funcs));
  if (!funcs) {
    fclose(out);
    return false;
  }
  size_t idx = 0;
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_FUNC) {
      funcs[idx++] = &decl->as.func_decl;
    }
  }
  bool has_main = false;
  for (size_t i = 0; i < func_count; ++i) {
    const AstFuncDecl* fn = funcs[i];
    
    // Check if we already emitted this function name + first param type combo
    bool already_emitted = false;
    Str first_type = {0};
    if (fn->params) first_type = get_base_type_name(fn->params->type);

    for (size_t j = 0; j < i; j++) {
        Str j_first_type = {0};
        if (funcs[j]->params) j_first_type = get_base_type_name(funcs[j]->params->type);

        if (str_eq(funcs[j]->name, fn->name) && str_eq(j_first_type, first_type)) {
            already_emitted = true;
            break;
        }
    }
    if (already_emitted) continue;

    if (str_eq_cstr(fn->name, "main")) {
      has_main = true;
      continue;
    }
    
    const char* ray_mapping = find_raylib_mapping(fn->name);
    if (ray_mapping) {
        if (!emit_raylib_wrapper(fn, ray_mapping, out, module)) {
            ok = false;
            break;
        }
        continue;
    }

    CFuncContext temp_ctx = {.module = module, .generic_params = fn->generic_params, .uses_raylib = uses_raylib};
    const char* return_type = c_return_type(&temp_ctx, fn);
    if (!return_type) {
      ok = false;
      break;
    }
    
    const char* qualifier = fn->is_extern ? "extern" : "RAE_UNUSED static";
    fprintf(out, "%s %s ", qualifier, return_type);
    emit_mangled_function_name(fn, out);
    fprintf(out, "(");

    if (!emit_param_list(&temp_ctx, fn->params, out, fn->is_extern)) {
      ok = false;
      break;
    }
    if (fprintf(out, ");\n") < 0) {
      ok = false;
      break;
    }
  }


  if (ok && func_count > 1) {
    ok = fprintf(out, "\n") >= 0;
  }
  
  // Track emitted bodies to avoid duplicates
  for (size_t i = 0; ok && i < func_count; ++i) {
    if (funcs[i]->is_extern) {
      continue;
    }
    
    bool body_emitted = false;

    for (size_t j = 0; j < i; j++) {
        const AstFuncDecl* fi = funcs[i];
        const AstFuncDecl* fj = funcs[j];
        if (str_eq(fj->name, fi->name) && !fj->is_extern) {
            // Count params for both
            uint16_t c_i = 0; for (const AstParam* p = fi->params; p; p = p->next) c_i++;
            uint16_t c_j = 0; for (const AstParam* p = fj->params; p; p = p->next) c_j++;
            
            if (c_i == c_j) {
                bool mismatch = false;
                const AstParam* pi = fi->params;
                const AstParam* pj = fj->params;
                while (pi && pj) {
                    if (!str_eq(get_base_type_name(pi->type), get_base_type_name(pj->type))) {
                        mismatch = true;
                        break;
                    }
                    pi = pi->next;
                    pj = pj->next;
                }
                if (!mismatch) {
                    body_emitted = true;
                    break;
                }
            }
        }
    }
    if (body_emitted) continue;

    ok = emit_function(module, funcs[i], out, registry, uses_raylib);
    if (!ok) break;
  }

  if (diag_error_count() > 0) ok = false;

  if (ok && !has_main) {
    fprintf(stderr, "error: C backend could not find `func main` in project\n");
    ok = false;
  }
  free(funcs);
  fclose(out);
  return ok;
}

static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_loop(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_match(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool is_wildcard_pattern(const AstExpr* expr);
static bool is_string_literal_expr(const AstExpr* expr);

static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  if (!stmt->as.if_stmt.condition || !stmt->as.if_stmt.then_block) {
    fprintf(stderr, "error: C backend requires complete if statements\n");
    return false;
  }
  if (fprintf(out, "  if (") < 0) {
    return false;
  }
  if (!emit_expr(ctx, stmt->as.if_stmt.condition, out, PREC_LOWEST, false)) {
    return false;
  }
  if (fprintf(out, ") {\n") < 0) {
    return false;
  }
  if (!emit_block(ctx, stmt->as.if_stmt.then_block, out)) {
    return false;
  }
  if (fprintf(out, "  }") < 0) {
    return false;
  }
  if (stmt->as.if_stmt.else_block) {
    if (fprintf(out, " else {\n") < 0) {
      return false;
    }
    if (!emit_block(ctx, stmt->as.if_stmt.else_block, out)) {
      return false;
    }
    if (fprintf(out, "  }") < 0) {
      return false;
    }
  }
  if (fprintf(out, "\n") < 0) {
    return false;
  }
  return true;
}

static bool emit_loop(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  if (stmt->as.loop_stmt.is_range) {
    fprintf(stderr, "warning: range loops not yet supported in C backend (skipping body)\n");
    return fprintf(out, "  /* range loop skipped */\n") >= 0;
  }

  // Wrap in scope to handle init variable lifetime
  if (fprintf(out, "  {\n") < 0) return false;
  ctx->scope_depth++;

  if (stmt->as.loop_stmt.init) {
    if (!emit_stmt(ctx, stmt->as.loop_stmt.init, out)) {
      ctx->scope_depth--;
      return false;
    }
  }

  if (fprintf(out, "  while (") < 0) {
    ctx->scope_depth--;
    return false;
  }
  if (stmt->as.loop_stmt.condition) {
    if (!emit_expr(ctx, stmt->as.loop_stmt.condition, out, PREC_LOWEST, false)) {
      ctx->scope_depth--;
      return false;
    }
  } else {
    if (fprintf(out, "1") < 0) {
      ctx->scope_depth--;
      return false;
    }
  }
  if (fprintf(out, ") {\n") < 0) {
    ctx->scope_depth--;
    return false;
  }

  if (!emit_block(ctx, stmt->as.loop_stmt.body, out)) {
    ctx->scope_depth--;
    return false;
  }

  if (stmt->as.loop_stmt.increment) {
    if (fprintf(out, "  ") < 0) {
      ctx->scope_depth--;
      return false;
    }
    if (!emit_expr(ctx, stmt->as.loop_stmt.increment, out, PREC_LOWEST, false)) {
      ctx->scope_depth--;
      return false;
    }
    if (fprintf(out, ";\n") < 0) {
      ctx->scope_depth--;
      return false;
    }
  }

  if (fprintf(out, "  }\n") < 0) {
    ctx->scope_depth--;
    return false;
  }
  
  if (!emit_defers(ctx, ctx->scope_depth, out)) {
    ctx->scope_depth--;
    return false;
  }
  pop_defers(ctx, ctx->scope_depth);
  ctx->scope_depth--;

  if (fprintf(out, "  }\n") < 0) return false;
  return true;
}

static bool is_wildcard_pattern(const AstExpr* expr) {
  return expr && expr->kind == AST_EXPR_IDENT && str_eq_cstr(expr->as.ident, "_");
}

static bool is_string_literal_expr(const AstExpr* expr) {
  return expr && expr->kind == AST_EXPR_STRING;
}

static bool match_cases_use_string(const AstMatchCase* cases, bool* out_use_string) {
  bool saw_string = false;
  bool saw_other = false;
  while (cases) {
    if (cases->pattern) {
      if (is_string_literal_expr(cases->pattern)) {
        saw_string = true;
      } else {
        saw_other = true;
      }
    }
    cases = cases->next;
  }
  if (saw_string && saw_other) {
    fprintf(stderr, "error: match cases mixing string and non-string patterns are unsupported\n");
    return false;
  }
  *out_use_string = saw_string;
  return true;
}

static bool match_arms_use_string(const AstMatchArm* arms, bool* out_use_string) {
  bool saw_string = false;
  bool saw_other = false;
  while (arms) {
    if (arms->pattern) {
      if (is_string_literal_expr(arms->pattern)) {
        saw_string = true;
      } else {
        saw_other = true;
      }
    }
    arms = arms->next;
  }
  if (saw_string && saw_other) {
    fprintf(stderr, "error: match expression arms mixing string and non-string patterns are unsupported\n");
    return false;
  }
  *out_use_string = saw_string;
  return true;
}

static bool emit_match(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  if (!stmt->as.match_stmt.subject || !stmt->as.match_stmt.cases) {
    fprintf(stderr, "error: C backend requires match subject and at least one case\n");
    return false;
  }
  bool use_string = false;
  if (!match_cases_use_string(stmt->as.match_stmt.cases, &use_string)) {
    return false;
  }
  
  bool is_ptr = false;
  if (stmt->as.match_stmt.subject->kind == AST_EXPR_IDENT) {
      if (is_pointer_type(ctx, stmt->as.match_stmt.subject->as.ident)) {
          is_ptr = true;
      }
  }

  size_t temp_id = ctx->temp_counter++;
  Str subject_type = infer_expr_type(ctx, stmt->as.match_stmt.subject);
  bool is_any = str_eq_cstr(subject_type, "Any");
  
  if (!is_any && ctx && ctx->generic_params) {
      const AstIdentifierPart* gp = ctx->generic_params;
      while (gp) {
          if (str_eq(gp->text, subject_type)) { is_any = true; break; }
          gp = gp->next;
      }
  }
  
  // Also check if it's a generic function call returning its generic parameter
  if (!is_any && (stmt->as.match_stmt.subject->kind == AST_EXPR_CALL || stmt->as.match_stmt.subject->kind == AST_EXPR_METHOD_CALL)) {
      const AstExpr* call = stmt->as.match_stmt.subject;
      const AstFuncDecl* d = NULL;
      if (call->kind == AST_EXPR_CALL) {
          if (call->as.call.callee->kind == AST_EXPR_IDENT) {
              Str name = call->as.call.callee->as.ident;
              uint16_t arg_count = 0;
              for (const AstCallArg* arg = call->as.call.args; arg; arg = arg->next) arg_count++;
              Str* arg_types = NULL;
              if (arg_count > 0) {
                  arg_types = malloc(arg_count * sizeof(Str));
                  const AstCallArg* arg = call->as.call.args;
                  for (uint16_t i = 0; i < arg_count; ++i) {
                      arg_types[i] = infer_expr_type(ctx, arg->value);
                      arg = arg->next;
                  }
              }
              d = find_function_overload(ctx->module, ctx, name, arg_types, arg_count);
              free(arg_types);
          }
      } else {
          // Method call - simplified
          Str mname = call->as.method_call.method_name;
          Str obj_type = infer_expr_type(ctx, call->as.method_call.object);
          const AstDecl* td = find_type_decl(ctx->module, obj_type);
          if (td && td->kind == AST_DECL_TYPE) {
              // ... would need method lookup
          }
          // HACK: common generic methods
          if (str_eq_cstr(mname, "get") || str_eq_cstr(mname, "pop")) is_any = true;
      }
      
      if (d && d->returns && d->returns->type && d->returns->type->parts) {
          Str rtype = d->returns->type->parts->text;
          const AstIdentifierPart* gp = d->generic_params;
          while (gp) {
              if (str_eq(gp->text, rtype)) { is_any = true; break; }
              gp = gp->next;
          }
      }
  }

  bool is_opt_subj = false;
  if (stmt->as.match_stmt.subject->kind == AST_EXPR_IDENT) {
      const AstTypeRef* tr = get_local_type_ref(ctx, stmt->as.match_stmt.subject->as.ident);
      if (tr && tr->is_opt) is_opt_subj = true;
  }

  if (use_string) {
    if (fprintf(out, "  const char* __match%zu = ", temp_id) < 0) return false;
  } else if (is_any || is_opt_subj) {
    if (fprintf(out, "  RaeAny __match%zu = ", temp_id) < 0) return false;
  } else if (is_ptr) {
    if (fprintf(out, "  void* __match%zu = ", temp_id) < 0) return false;
  } else {
    if (fprintf(out, "  int64_t __match%zu = ", temp_id) < 0) return false;
  }
  if (is_any || is_opt_subj) fprintf(out, "rae_any(");
  if (!emit_expr(ctx, stmt->as.match_stmt.subject, out, PREC_LOWEST, false)) {
    return false;
  }
  if (is_any || is_opt_subj) fprintf(out, ")");
  if (fprintf(out, ";\n") < 0) {
    return false;
  }
  const AstMatchCase* current = stmt->as.match_stmt.cases;
  const AstMatchCase* default_case = NULL;
  int case_index = 0;

  while (current) {
    if (!current->pattern || is_wildcard_pattern(current->pattern)) {
      if (default_case) {
        fprintf(stderr, "error: multiple default cases in match\n");
        return false;
      }
      default_case = current;
      current = current->next;
      continue;
    }
    if (use_string) {
      if (fprintf(out, "%s(__match%zu && strcmp(__match%zu, ", case_index > 0 ? " else if " : "  if ", temp_id, temp_id) < 0) {
        return false;
      }
    } else if (is_any || is_opt_subj) {
      if (current->pattern && current->pattern->kind == AST_EXPR_NONE) {
          if (fprintf(out, "%s(__match%zu.type == RAE_TYPE_NONE", case_index > 0 ? " else if " : "  if ", temp_id) < 0) return false;
      } else {
          // General Any comparison not fully supported, but we'll try something basic
          if (fprintf(out, "%s(__match%zu.as.i == ", case_index > 0 ? " else if " : "  if ", temp_id) < 0) return false;
      }
    } else {
      if (fprintf(out, "%s(__match%zu == ", case_index > 0 ? " else if " : "  if ", temp_id) < 0) {
        return false;
      }
    }
    
    if ((is_any || is_opt_subj) && current->pattern && current->pattern->kind == AST_EXPR_NONE) {
        // Skip emitting 0 for none since we already handled it
    } else {
        if (!emit_expr(ctx, current->pattern, out, PREC_LOWEST, false)) {
          return false;
        }
    }

    if (use_string) {
      if (fprintf(out, ") == 0) {\n") < 0) {
        return false;
      }
    } else {
      if (fprintf(out, ") {\n") < 0) {
        return false;
      }
    }
    if (!emit_block(ctx, current->block, out)) {
      return false;
    }
    if (fprintf(out, "  }") < 0) {
      return false;
    }
    case_index += 1;
    current = current->next;
  }
  if (default_case) {
    if (case_index > 0) {
      if (fprintf(out, " else {\n") < 0) {
        return false;
      }
      if (!emit_block(ctx, default_case->block, out)) {
        return false;
      }
      if (fprintf(out, "  }") < 0) {
        return false;
      }
    } else {
      if (!emit_block(ctx, default_case->block, out)) {
        return false;
      }
    }
  }
  if (case_index > 0) {
    if (fprintf(out, "\n") < 0) {
      return false;
    }
  }
  return true;
}

