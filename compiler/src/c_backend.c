#include "c_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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
} CFuncContext;

// Forward declarations
static const char* find_raylib_mapping(Str name);
static const AstDecl* find_type_decl(const AstModule* module, Str name);
static const AstDecl* find_enum_decl(const AstModule* module, Str name);
static bool has_property(const AstProperty* props, const char* name);
static bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out, int parent_prec);
static bool emit_function(const AstModule* module, const AstFuncDecl* func, FILE* out);
static bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_call(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_log_call(CFuncContext* ctx, const AstExpr* expr, FILE* out, bool newline);
static bool emit_call_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_string_literal(FILE* out, Str literal);
static bool emit_param_list(CFuncContext* ctx, const AstParam* params, FILE* out);
static const char* map_rae_type_to_c(Str type_name);
static bool is_primitive_type(Str type_name);
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
  if (str_eq_cstr(type_name, "List")) return "List";
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
      } else {
          if (fprintf(out, "%.*s", (int)current->text.len, current->text.data) < 0) return false;
          
          while (current->next) { // Handle multi-part identifiers (e.g., Module.Type)
              current = current->next;
              if (fprintf(out, "_%.*s", (int)current->text.len, current->text.data) < 0) return false; // Concatenate with underscore
          }
      }
  }

  if (is_ptr) {
      if (fprintf(out, "*") < 0) return false;
  }

  if (type->generic_args) {
      // C backend doesn't currently support generic types directly in type names
      // For now, we'll just ignore them or emit a warning if needed
      // fprintf(stderr, "warning: C backend does not yet fully support generic type arguments\n");
  }

  return true;
}

static bool emit_param_list(CFuncContext* ctx, const AstParam* params, FILE* out) {
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
            is_ptr = param->type->is_view || param->type->is_mod;
            Str first = param->type->parts->text;
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
                } else {
                    c_type_base = free_me = str_to_cstr(first);
                }
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
    bool is_ptr = func->returns->type->is_view || func->returns->type->is_mod;
    
    if (func->returns->type->is_id) return "int64_t";
    if (func->returns->type->is_key) return "const char*";

    const char* mapped = map_rae_type_to_c(func->returns->type->parts->text);
    if (mapped) {
        if (is_ptr) {
            // HACK: this will leak or produce weird results if mapped is "const char*"
            // but for now it helps with "mod T" returns.
            char* buf = malloc(strlen(mapped) + 2);
            sprintf(buf, "%s*", mapped);
            return buf; // leak
        }
        return mapped;
    }

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

    char* type_name = str_to_cstr(func->returns->type->parts->text);
    if (is_ptr) {
        char* buf = malloc(strlen(type_name) + 2);
        sprintf(buf, "%s*", type_name);
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

static Str infer_buffer_element_type(CFuncContext* ctx, const AstExpr* expr) {
    if (expr->kind == AST_EXPR_IDENT) {
        const AstTypeRef* type = get_local_type_ref(ctx, expr->as.ident);
        if (type && type->parts && str_eq_cstr(type->parts->text, "Buffer")) {
            if (type->generic_args && type->generic_args->parts) {
                return type->generic_args->parts->text;
            }
        }
    } else if (expr->kind == AST_EXPR_MEMBER) {
        Str obj_type = infer_expr_type(ctx, expr->as.member.object);
        if (obj_type.len > 0) {
            const AstDecl* d = find_type_decl(ctx->module, obj_type);
            if (d && d->kind == AST_DECL_TYPE) {
                for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
                    if (str_eq(f->name, expr->as.member.member)) {
                        if (f->type && f->type->parts && str_eq_cstr(f->type->parts->text, "Buffer")) {
                            if (f->type->generic_args && f->type->generic_args->parts) {
                                return f->type->generic_args->parts->text;
                            }
                        }
                    }
                }
            }
        }
    }
    return (Str){0};
}

static Str infer_expr_type(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr) return (Str){0};
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
            }
            break;
        default: break;
    }
done:
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
    if (fprintf(out, "  %s(", newline ? "rae_ext_rae_log_list_fields" : "rae_ext_rae_log_stream_list_fields") < 0) return false;
    if (!emit_expr(ctx, value, out, PREC_LOWEST)) return false;
    fprintf(out, ".data, ");
    if (!emit_expr(ctx, value, out, PREC_LOWEST)) return false;
    fprintf(out, ".length, ");
    if (!emit_expr(ctx, value, out, PREC_LOWEST)) return false;
    fprintf(out, ".capacity);\n");
    return true;
  }

  bool is_generic = strcmp(log_fn, "rae_ext_rae_log_any") == 0 || strcmp(log_fn, "rae_ext_rae_log_stream_any") == 0;
  bool is_enum = find_enum_decl(ctx->module, type_name) != NULL;

  if (fprintf(out, "  %s(", log_fn) < 0) return false;
  if (is_generic) fprintf(out, "rae_any(");
  if (is_enum) fprintf(out, "(int64_t)(");
  if (val_is_ptr) fprintf(out, "(*");
  if (!emit_expr(ctx, value, out, PREC_LOWEST)) return false;
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
    fprintf(out, "_");
    for (const AstParam* p = func->params; p; p = p->next) {
        Str p_type = get_base_type_name(p->type);
        fprintf(out, "%.*s_", (int)p_type.len, p_type.data);
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
          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST)) return false;
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
          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST)) return false;
          fprintf(out, ")");
          return true;
      }
      if (str_eq_cstr(name, "__buf_resize")) {
          const AstCallArg* arg = expr->as.call.args;
          if (!arg || !arg->next) return false;
          fprintf(out, "rae_ext_rae_buf_resize(");
          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST)) return false;
          fprintf(out, ", ");
          if (!emit_expr(ctx, arg->next->value, out, PREC_LOWEST)) return false;
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
          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST)) return false; // src
          fprintf(out, ", ");
          if (!emit_expr(ctx, arg->next->value, out, PREC_LOWEST)) return false; // src_off
          fprintf(out, ", ");
          if (!emit_expr(ctx, arg->next->next->value, out, PREC_LOWEST)) return false; // dst
          fprintf(out, ", ");
          if (!emit_expr(ctx, arg->next->next->next->value, out, PREC_LOWEST)) return false; // dst_off
          fprintf(out, ", ");
          if (!emit_expr(ctx, arg->next->next->next->next->value, out, PREC_LOWEST)) return false; // len
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
          Str item_type = infer_buffer_element_type(ctx, arg->value);
          const char* inner_c = "int64_t";
          if (str_eq_cstr(item_type, "Any")) inner_c = "RaeAny";
          else if (item_type.len > 0 && !is_primitive_type(item_type)) {
              bool is_generic_param = false;
              if (ctx && ctx->generic_params) {
                  const AstIdentifierPart* gp = ctx->generic_params;
                  while (gp) {
                      if (str_eq(gp->text, item_type)) { is_generic_param = true; break; }
                      gp = gp->next;
                  }
              }
              if (is_generic_param) {
                  inner_c = "RaeAny";
              } else {
                  char* tn = str_to_cstr(item_type);
                  inner_c = tn; // leak
              }
          }
          fprintf(out, "((%s*)(", inner_c);
          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST)) return false;
          fprintf(out, "))[");
          if (!emit_expr(ctx, arg->next->value, out, PREC_LOWEST)) return false;
          fprintf(out, "]");
          return true;
      }
      if (str_eq_cstr(name, "__buf_set")) {
          const AstCallArg* arg = expr->as.call.args;
          if (!arg || !arg->next || !arg->next->next) return false;
          Str item_type = infer_buffer_element_type(ctx, arg->value);
          const char* inner_c = "int64_t";
          if (str_eq_cstr(item_type, "Any")) inner_c = "RaeAny";
          else if (item_type.len > 0 && !is_primitive_type(item_type)) {
              bool is_generic_param = false;
              if (ctx && ctx->generic_params) {
                  const AstIdentifierPart* gp = ctx->generic_params;
                  while (gp) {
                      if (str_eq(gp->text, item_type)) { is_generic_param = true; break; }
                      gp = gp->next;
                  }
              }
              if (is_generic_param) {
                  inner_c = "RaeAny";
              } else {
                  char* tn = str_to_cstr(item_type);
                  inner_c = tn; // leak
              }
          }
          fprintf(out, "((%s*)(", inner_c);
          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST)) return false;
          fprintf(out, "))[");
          if (!emit_expr(ctx, arg->next->value, out, PREC_LOWEST)) return false;
          fprintf(out, "] = ");
          if (strcmp(inner_c, "RaeAny") == 0) fprintf(out, "rae_any(");
          else if (arg->next->next->value->kind == AST_EXPR_OBJECT && !is_primitive_type(item_type)) {
              fprintf(out, "(%s)", inner_c);
          }
          if (!emit_expr(ctx, arg->next->next->value, out, PREC_LOWEST)) return false;
          if (strcmp(inner_c, "RaeAny") == 0) fprintf(out, ")");
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
      free(arg_types);

      if (!func_decl && !find_raylib_mapping(callee->as.ident)) {
          char buffer[128];
          snprintf(buffer, sizeof(buffer), "unknown function '%.*s' for VM call", (int)callee->as.ident.len, callee->as.ident.data);
          diag_error(ctx->module->file_path, (int)expr->line, (int)expr->column, buffer);
          return false;
      }
  }

  // Handle return type casting if it's a generic return
  const char* cast_pre = "";
  const char* cast_post = "";
  if (func_decl && func_decl->returns && func_decl->returns->type && func_decl->returns->type->parts) {
      Str rtype = func_decl->returns->type->parts->text;
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

  fprintf(out, "%s", cast_pre);

  if (func_decl && !str_eq_cstr(func_decl->name, "main")) {
      emit_mangled_function_name(func_decl, out);
  } else if (callee->kind == AST_EXPR_IDENT) {
      const char* ray_mapping = find_raylib_mapping(callee->as.ident);
      if (ray_mapping) {
          fprintf(out, "rae_ext_%.*s", (int)callee->as.ident.len, callee->as.ident.data);
      } else {
          if (!emit_expr(ctx, callee, out, PREC_CALL)) return false;
      }
  } else {
      if (!emit_expr(ctx, callee, out, PREC_CALL)) return false;
  }

  if (fprintf(out, "(") < 0) return false;

  
  const AstCallArg* arg = expr->as.call.args;
  const AstParam* param = func_decl ? func_decl->params : NULL;
  bool first = true;
  
  while (arg) {
    if (!first) {
      if (fprintf(out, ", ") < 0) return false;
    }
    first = false;
    
    bool needs_addr = false;
    bool is_any_param = false;
    
    if (func_decl) {
        // We have signature!
        if (param) {
             // If param expects ptr (view/mod), we need addr
             if (param->type && (param->type->is_view || param->type->is_mod)) {
                 needs_addr = true;
             }
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
             // We'll advance 'param' at the end of the loop to be safe with all branches
        }
    } else {
        // Fallback heuristic
        if (arg->value->kind == AST_EXPR_IDENT) {
            Str type_name = get_local_type_name(ctx, arg->value->as.ident);
            if (type_name.len > 0 && !is_primitive_type(type_name)) {
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
             if (!emit_expr(ctx, arg->value, out, PREC_LOWEST)) return false;
             fprintf(out, " })");
             
             // Advance to next argument skipping normal emission
             arg = arg->next;
             if (func_decl && param) param = param->next;
             continue;
         }
         
         if (fprintf(out, "&(") < 0) return false;
    } else if (!needs_addr && have_ptr) {
         if (fprintf(out, "*(") < 0) return false;
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

    if (is_c_struct && !needs_addr) {
        // If it's already an object of the right type (explicitly tagged), just emit it
        if (arg->value->kind == AST_EXPR_OBJECT && arg->value->as.object_literal.type) {
            if (!emit_expr(ctx, arg->value, out, PREC_LOWEST)) return false;
        } else if (arg->value->kind == AST_EXPR_OBJECT) {
            // Manual field mapping for untyped literals
            fprintf(out, "(%.*s){ ", (int)target_type.len, target_type.data);
            if (type_decl) {
                const AstTypeField* field = type_decl->as.type_decl.fields;
                while (field) {
                    fprintf(out, ".%.*s = ", (int)field->name.len, field->name.data);
                    bool found_field = false;
                    const AstObjectField* of = arg->value->as.object_literal.fields;
                    while (of) {
                        if (str_eq(of->name, field->name)) {
                            if (!emit_expr(ctx, of->value, out, PREC_LOWEST)) return false;
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
                    if (!emit_expr(ctx, of->value, out, PREC_LOWEST)) return false;
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
            if (!emit_expr(ctx, arg->value, out, PREC_LOWEST)) return false;
        }
    } else {
        if (is_any_param && !needs_addr) {
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

        if (!emit_expr(ctx, arg->value, out, PREC_LOWEST)) {
          return false;
        }
        
        if (is_any_param && !needs_addr) {
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
    }

    if ((needs_addr && !have_ptr) || (!needs_addr && have_ptr)) {
         if (fprintf(out, ")") < 0) return false;
    }
    
    arg = arg->next;
    if (func_decl && param) param = param->next;
  }
  fprintf(out, ")");
  fprintf(out, "%s", cast_post);
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

static bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out, int parent_prec) {
  if (!expr) return false;
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
          if (!emit_expr(ctx, part->value, out, PREC_LOWEST)) return false;
          if (is_enum) fprintf(out, ")");
          fprintf(out, ")");
      } else {
          if (!emit_expr(ctx, part->value, out, PREC_LOWEST)) return false;
      }
      part = part->next;
      
      while (part) {
          fprintf(out, ", ");
          if (part->value->kind != AST_EXPR_STRING) {
              Str type_name = infer_expr_type(ctx, part->value);
              bool is_enum = find_enum_decl(ctx->module, type_name) != NULL;
              fprintf(out, "rae_ext_rae_str(");
              if (is_enum) fprintf(out, "(int64_t)(");
              if (!emit_expr(ctx, part->value, out, PREC_LOWEST)) return false;
              if (is_enum) fprintf(out, ")");
              fprintf(out, ")");
          } else {
              if (!emit_expr(ctx, part->value, out, PREC_LOWEST)) return false;
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
              if (!emit_expr(ctx, expr->as.binary.lhs, out, PREC_LOWEST)) return false;
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
                if (!emit_expr(ctx, expr->as.binary.lhs, out, PREC_LOWEST)) return false;
                fprintf(out, ", ");
                if (!emit_expr(ctx, expr->as.binary.rhs, out, PREC_LOWEST)) return false;
                fprintf(out, ")");
            } else {
                if (need_paren) fprintf(out, "(");
                if (!emit_expr(ctx, expr->as.binary.lhs, out, prec)) return false;
                fprintf(out, " %% ");
                if (!emit_expr(ctx, expr->as.binary.rhs, out, prec + 1)) return false;
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
              if (!emit_expr(ctx, expr->as.binary.lhs, out, prec)) return false;
              if (fprintf(out, ") ? rae_ext_rae_str(") < 0) return false;
              if (!emit_expr(ctx, expr->as.binary.lhs, out, prec)) return false;
              if (fprintf(out, ") : \"\"), (rae_ext_rae_str(") < 0) return false;
              if (!emit_expr(ctx, expr->as.binary.rhs, out, prec)) return false;
              if (fprintf(out, ") ? rae_ext_rae_str(") < 0) return false;
              if (!emit_expr(ctx, expr->as.binary.rhs, out, prec)) return false;
              if (fprintf(out, ") : \"\")) == 0)") < 0) return false;
              return true;
          }
      }

      if (need_paren && fprintf(out, "(") < 0) return false;
      
      bool lhs_is_ptr = (expr->as.binary.lhs->kind == AST_EXPR_IDENT && is_pointer_type(ctx, expr->as.binary.lhs->as.ident));
      if (lhs_is_ptr) fprintf(out, "(*");
      if (!emit_expr(ctx, expr->as.binary.lhs, out, prec)) return false;
      if (lhs_is_ptr) fprintf(out, ")");
      
      if (fprintf(out, " %s ", op) < 0) return false;
      
      bool rhs_is_ptr = (expr->as.binary.rhs->kind == AST_EXPR_IDENT && is_pointer_type(ctx, expr->as.binary.rhs->as.ident));
      if (rhs_is_ptr) fprintf(out, "(*");
      if (!emit_expr(ctx, expr->as.binary.rhs, out, prec + 1)) return false;
      if (rhs_is_ptr) fprintf(out, ")");
      
      if (need_paren && fprintf(out, ")") < 0) return false;
      return true;
    }
    case AST_EXPR_UNARY:
      if (expr->as.unary.op == AST_UNARY_NEG) {
        if (fprintf(out, "(-") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY)) return false;
        return fprintf(out, ")") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_NOT) {
        if (fprintf(out, "(!(") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY)) return false;
        return fprintf(out, "))") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_PRE_INC) {
        if (fprintf(out, "++") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY)) return false;
        return true;
      } else if (expr->as.unary.op == AST_UNARY_PRE_DEC) {
        if (fprintf(out, "--") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY)) return false;
        return true;
      } else if (expr->as.unary.op == AST_UNARY_POST_INC) {
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY)) return false;
        return fprintf(out, "++") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_POST_DEC) {
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY)) return false;
        return fprintf(out, "--") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_VIEW || expr->as.unary.op == AST_UNARY_MOD) {
        // In C backend, references are pointers. We take the address of the operand.
        if (fprintf(out, "(&(") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY)) return false;
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
      if (!emit_expr(ctx, expr->as.match_expr.subject, out, PREC_LOWEST)) {
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
          if (!emit_expr(ctx, arm->value, out, PREC_LOWEST)) {
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
          if (!emit_expr(ctx, arm->pattern, out, PREC_LOWEST)) {
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
          if (!emit_expr(ctx, arm->value, out, PREC_LOWEST)) {
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
          if (!emit_expr(ctx, expr->as.member.object, out, PREC_CALL)) return false;
          fprintf(out, ")%sas.%s)", sep, union_field);
          return true;
      }

      if (!emit_expr(ctx, expr->as.member.object, out, PREC_CALL)) return false;
      return fprintf(out, "%s%.*s", sep, (int)expr->as.member.member.len, expr->as.member.member.data) >= 0;
    }
    case AST_EXPR_OBJECT: {
      const AstTypeDecl* type_decl = NULL;
      if (expr->as.object_literal.type) {
        fprintf(out, "(");
        if (!emit_type_ref_as_c_type(ctx, expr->as.object_literal.type, out)) return false;
        fprintf(out, ")");
        
        Str type_name = expr->as.object_literal.type->parts->text;
        const AstDecl* d = find_type_decl(ctx->module, type_name);
        if (d && d->kind == AST_DECL_TYPE) type_decl = &d->as.type_decl;
      } else {
          const AstTypeRef* type_hint = ctx->expected_type;
          if (type_hint && type_hint->parts) {
              Str type_name = type_hint->parts->text;
              const AstDecl* d = find_type_decl(ctx->module, type_name);
              if (d && d->kind == AST_DECL_TYPE) type_decl = &d->as.type_decl;
          }
          
          if (!type_decl) {
              // If no explicit type, try to infer it from the expression itself (e.g. via expected type in context)
              Str inferred = infer_expr_type(ctx, expr);
              if (inferred.len > 0) {
                  const AstDecl* d = find_type_decl(ctx->module, inferred);
                  if (d && d->kind == AST_DECL_TYPE) type_decl = &d->as.type_decl;
              }
          }
      }
      
      if (fprintf(out, "{ ") < 0) return false;
      const AstObjectField* field = expr->as.object_literal.fields;
      while (field) {
        if (field->name.len > 0) {
            if (fprintf(out, ".%.*s = ", (int)field->name.len, field->name.data) < 0) return false;
        }
        
        bool is_any_field = false;
        if (type_decl) {
            const AstTypeField* tf = type_decl->fields;
            while (tf) {
                if (str_eq(tf->name, field->name)) {
                    if (tf->type && tf->type->parts) {
                        Str ftype = tf->type->parts->text;
                        if (str_eq_cstr(ftype, "Any")) is_any_field = true;
                        else {
                            // Check generic params of the TYPE declaration
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
                    break;
                }
                tf = tf->next;
            }
        }
        
        if (is_any_field) fprintf(out, "rae_any(");
        if (!emit_expr(ctx, field->value, out, PREC_LOWEST)) return false;
        if (is_any_field) fprintf(out, ")");
        
        if (field->next) fprintf(out, ", ");
        field = field->next;
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
        if (!emit_expr(ctx, current->value, out, PREC_LOWEST)) return false;
        fprintf(out, ")); ");
        current = current->next;
      }
      fprintf(out, "_l; })");
      return true;
    }
    case AST_EXPR_INDEX: {
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
          if (!emit_expr(ctx, expr->as.index.target, out, PREC_LOWEST)) return false;
          if (needs_addr) fprintf(out, ")");
          
          fprintf(out, ", ");
          if (!emit_expr(ctx, expr->as.index.index, out, PREC_LOWEST)) return false;
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
        fprintf(out, "(");
        
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

        if (needs_addr && !have_ptr) fprintf(out, "&(");
        else if (!needs_addr && have_ptr) fprintf(out, "*(");
        
        if (!emit_expr(ctx, receiver, out, PREC_LOWEST)) return false;
        
        if ((needs_addr && !have_ptr) || (!needs_addr && have_ptr)) fprintf(out, ")");

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
          if (!emit_expr(ctx, arg->value, out, PREC_LOWEST)) return false;
          if (is_any_param) fprintf(out, ")");
          
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
          if (!emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST)) return false;
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
          if (!emit_expr(ctx, current->value, out, PREC_LOWEST)) return false;
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
      fprintf(out, "__extension__ ({ List _l = ");
      if (create_fn) emit_mangled_function_name(create_fn, out);
      else fprintf(out, "rae_createList_Int_");
      fprintf(out, "(%u); ", element_count);

      current = expr->as.collection.elements;
      while (current) {
        if (add_fn) {
            emit_mangled_function_name(add_fn, out);
        } else {
            fprintf(out, "rae_add_List_T_");
        }
        fprintf(out, "(&_l, rae_any(");
        if (!emit_expr(ctx, current->value, out, PREC_LOWEST)) return false;
        fprintf(out, ")); ");
        current = current->next;
      }
      fprintf(out, "_l; })");
      return true;
    }
  }
  fprintf(stderr, "error: unsupported expression kind in C backend\n");
  return false;
}

static bool emit_call(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
  if (expr->kind == AST_EXPR_METHOD_CALL) {
      if (fprintf(out, "  ") < 0) return false;
      if (!emit_expr(ctx, expr, out, PREC_LOWEST)) return false;
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
  if (!emit_expr(ctx, expr, out, PREC_LOWEST)) {
    return false;
  }
  if (fprintf(out, ";\n") < 0) {
    return false;
  }
  return true;
}

static bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  if (!stmt) return true;
  switch (stmt->kind) {
    case AST_STMT_RET: {
      const AstReturnArg* arg = stmt->as.ret_stmt.values;
      if (!arg) {
        if (ctx->returns_value) {
          fprintf(stderr, "error: return without value in function expecting a value\n");
          return false;
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
      if (fprintf(out, "  return ") < 0) {
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
              return fprintf(out, "rae_any_none();\n") >= 0;
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
      
      if (!emit_expr(ctx, arg->value, out, PREC_LOWEST)) {
        return false;
      }
      
      if (ret_is_any) fprintf(out, ")");
      return fprintf(out, ";\n") >= 0;
    }
        case AST_STMT_LET: {
          if (ctx->local_count >= sizeof(ctx->locals) / sizeof(ctx->locals[0])) {
            fprintf(stderr, "error: C backend local limit exceeded\n");
            return false;
          }
    
          fprintf(out, "  ");
          if (stmt->as.let_stmt.type) {
              if (!emit_type_ref_as_c_type(ctx, stmt->as.let_stmt.type, out)) return false;
          } else {
              fprintf(out, "int64_t");
          }
          
          fprintf(out, " %.*s", (int)stmt->as.let_stmt.name.len, stmt->as.let_stmt.name.data);

          if (!stmt->as.let_stmt.value) {
              fprintf(out, " = {0}");
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

              if (!emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST)) {
                ctx->expected_type = NULL;
                return false;
              }
              ctx->expected_type = NULL;
              if (val_needs_addr) {
                  if (fprintf(out, ")") < 0) return false;
              }
              if (is_any || is_opt) fprintf(out, ")");
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
        }    case AST_STMT_EXPR:
      return emit_call(ctx, stmt->as.expr_stmt, out);
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
          fprintf(out, "(*(");
          if (!emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_ASSIGN)) return false;
          fprintf(out, "))");
      } else {
          if (!emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_ASSIGN)) return false;
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
          ctx->expected_type = get_local_type_ref(ctx, stmt->as.assign_stmt.target->as.ident);
      }

      // Fix: If it's an object literal being assigned, we need a C cast (Type){...}
      if (stmt->as.assign_stmt.value->kind == AST_EXPR_OBJECT && !stmt->as.assign_stmt.value->as.object_literal.type) {
          Str target_type = infer_expr_type(ctx, stmt->as.assign_stmt.target);
          if (target_type.len > 0) {
              fprintf(out, "(%.*s)", (int)target_type.len, target_type.data);
          }
      }

      if (!emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST)) {
          ctx->expected_type = NULL;
          return false;
      }
      ctx->expected_type = NULL;
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

static bool emit_raylib_wrapper(const AstFuncDecl* fn, const char* c_name, FILE* out, const AstModule* module) {
  CFuncContext temp_ctx = {.module = module, .generic_params = fn->generic_params};
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
  if (!emit_param_list(&temp_ctx, fn->params, out)) return false;
  fprintf(out, ") {\n");
    
  if (strcmp(return_type, "void") != 0) fprintf(out, "  return ");
  else fprintf(out, "  ");
  
  fprintf(out, "%s(", c_name);
  const AstParam* p = fn->params;
  while (p) {
      Str type_name = get_base_type_name(p->type);
      const AstDecl* td = find_type_decl(module, type_name);
      
      if (td && td->kind == AST_DECL_TYPE && has_property(td->as.type_decl.properties, "c_struct")) {
          if (str_eq_cstr(type_name, "Color")) {
              fprintf(out, "(Color){ (unsigned char)%.*s.r, (unsigned char)%.*s.g, (unsigned char)%.*s.b, (unsigned char)%.*s.a }", 
                      (int)p->name.len, p->name.data, (int)p->name.len, p->name.data, (int)p->name.len, p->name.data, (int)p->name.len, p->name.data);
          } else if (str_eq_cstr(type_name, "Vector3")) {
              fprintf(out, "(Vector3){ (float)%.*s.x, (float)%.*s.y, (float)%.*s.z }", 
                      (int)p->name.len, p->name.data, (int)p->name.len, p->name.data, (int)p->name.len, p->name.data);
          } else if (str_eq_cstr(type_name, "Texture")) {
              fprintf(out, "(Texture){ .id = (unsigned int)%.*s.id, .width = (int)%.*s.width, .height = (int)%.*s.height, .mipmaps = (int)%.*s.mipmaps, .format = (int)%.*s.format }", 
                      (int)p->name.len, p->name.data, (int)p->name.len, p->name.data, (int)p->name.len, p->name.data, (int)p->name.len, p->name.data, (int)p->name.len, p->name.data);
          } else if (str_eq_cstr(type_name, "Camera3D")) {
              fprintf(out, "(Camera3D){ ");
              fprintf(out, ".position = (Vector3){ (float)%.*s.position.x, (float)%.*s.position.y, (float)%.*s.position.z }, ", 
                      (int)p->name.len, p->name.data, (int)p->name.len, p->name.data, (int)p->name.len, p->name.data);
              fprintf(out, ".target = (Vector3){ (float)%.*s.target.x, (float)%.*s.target.y, (float)%.*s.target.z }, ", 
                      (int)p->name.len, p->name.data, (int)p->name.len, p->name.data, (int)p->name.len, p->name.data);
              fprintf(out, ".up = (Vector3){ (float)%.*s.up.x, (float)%.*s.up.y, (float)%.*s.up.z }, ", 
                      (int)p->name.len, p->name.data, (int)p->name.len, p->name.data, (int)p->name.len, p->name.data);
              fprintf(out, ".fovy = (float)%.*s.fovy, .projection = (int)%.*s.projection ", 
                      (int)p->name.len, p->name.data, (int)p->name.len, p->name.data);
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

static bool emit_function(const AstModule* module, const AstFuncDecl* func, FILE* out) {
  if (func->is_extern) {
    return true;
  }
  if (!func->body) {
    fprintf(stderr, "error: C backend requires function bodies to be present\n");
    return false;
  }
  
  const AstIdentifierPart* generic_params = func->generic_params;
  
  bool is_main = str_eq_cstr(func->name, "main");
  
  CFuncContext temp_ctx = {.generic_params = generic_params};
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
    ok = emit_param_list(&temp_ctx, func->params, out);
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
                      .temp_counter = 0};
                      
  const AstStmt* stmt = func->body->first;
  while (stmt && ok) {
    ok = emit_stmt(&ctx, stmt, out);
    stmt = stmt->next;
  }
  
  if (ok && is_main) {
    ok = fprintf(out, "  return 0;\n") >= 0;
  }
  
  if (ok) {
    ok = fprintf(out, "}\n\n") >= 0;
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

static const AstDecl* find_type_decl(const AstModule* module, Str name) {
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_TYPE && str_eq(decl->as.type_decl.name, name)) {
      return decl;
    }
  }
  return NULL;
}

static const AstDecl* find_enum_decl(const AstModule* module, Str name) {
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_ENUM && str_eq(decl->as.enum_decl.name, name)) {
      return decl;
    }
  }
  return NULL;
}

static bool emit_single_struct_def(const AstModule* module, const AstDecl* decl, FILE* out, 
                                   Str* emitted_types, size_t* emitted_count) {
  // Check if already emitted
  for (size_t i = 0; i < *emitted_count; ++i) {
    if (str_eq(emitted_types[i], decl->as.type_decl.name)) return true;
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
      if (!is_primitive_type(type_name)) {
        const AstDecl* dep = find_type_decl(module, type_name);
        if (dep) {
          if (!emit_single_struct_def(module, dep, out, emitted_types, emitted_count)) return false;
        }
      }
    }
    field = field->next;
  }

  // Now emit this struct
  char* name = str_to_cstr(decl->as.type_decl.name);
  if (fprintf(out, "typedef struct {\n") < 0) { free(name); return false; }
  
  CFuncContext temp_ctx = {.generic_params = decl->as.type_decl.generic_params};

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

static bool emit_enum_defs(const AstModule* module, FILE* out) {
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_ENUM) {
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

static bool emit_struct_defs(const AstModule* module, FILE* out) {
  // Simple array to track emitted types
  Str emitted_types[256]; // simplistic limit
  size_t emitted_count = 0;

  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_TYPE) {
      if (!emit_single_struct_def(module, decl, out, emitted_types, &emitted_count)) return false;
    }
  }
  return true;
}

bool c_backend_emit_module(const AstModule* module, const char* out_path) {
  if (!module || !out_path) return false;
  FILE* out = fopen(out_path, "w");
  if (!out) {
    fprintf(stderr, "error: unable to open '%s' for C backend output\n", out_path);
    return false;
  }
  bool ok = true;
  if (fprintf(out, "/* Generated by Rae C backend (experimental) */\n") < 0) return false;
  if (fprintf(out, "#include <stdint.h>\n") < 0) return false;
  if (fprintf(out, "#include <string.h>\n") < 0) return false;
  
  // Pre-scan for Raylib usage
  bool uses_raylib = false;
  const AstDecl* scan = module->decls;
  while (scan) {
      if (scan->kind == AST_DECL_FUNC) {
          if (find_raylib_mapping(scan->as.func_decl.name)) {
              uses_raylib = true;
              break;
          }
      }
      scan = scan->next;
  }
  
  if (uses_raylib) {
      if (fprintf(out, "#ifndef RAE_HAS_RAYLIB\n") < 0) return false;
      if (fprintf(out, "#define RAE_HAS_RAYLIB\n") < 0) return false;
      if (fprintf(out, "#endif\n") < 0) return false;
      if (fprintf(out, "#include <raylib.h>\n") < 0) return false;
  }
  
  if (fprintf(out, "#include \"rae_runtime.h\"\n\n") < 0) return false;

  if (!emit_enum_defs(module, out)) {
    fclose(out);
    return false;
  }

  if (!emit_struct_defs(module, out)) {
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

    CFuncContext temp_ctx = {.generic_params = fn->generic_params};
    const char* return_type = c_return_type(&temp_ctx, fn);
    if (!return_type) {
      ok = false;
      break;
    }
    
    const char* qualifier = fn->is_extern ? "extern" : "RAE_UNUSED static";
    fprintf(out, "%s %s ", qualifier, return_type);
    emit_mangled_function_name(fn, out);
    fprintf(out, "(");

    if (!emit_param_list(&temp_ctx, fn->params, out)) {
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

    ok = emit_function(module, funcs[i], out);
  }
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
  if (!emit_expr(ctx, stmt->as.if_stmt.condition, out, PREC_LOWEST)) {
    return false;
  }
  if (fprintf(out, ") {\n") < 0) {
    return false;
  }
  const AstStmt* inner = stmt->as.if_stmt.then_block->first;
  while (inner) {
    if (!emit_stmt(ctx, inner, out)) {
      return false;
    }
    inner = inner->next;
  }
  if (fprintf(out, "  }") < 0) {
    return false;
  }
  if (stmt->as.if_stmt.else_block) {
    if (fprintf(out, " else {\n") < 0) {
      return false;
    }
    const AstStmt* else_stmt = stmt->as.if_stmt.else_block->first;
    while (else_stmt) {
      if (!emit_stmt(ctx, else_stmt, out)) {
        return false;
      }
      else_stmt = else_stmt->next;
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

  if (stmt->as.loop_stmt.init) {
    if (!emit_stmt(ctx, stmt->as.loop_stmt.init, out)) {
      return false;
    }
  }

  if (fprintf(out, "  while (") < 0) return false;
  if (stmt->as.loop_stmt.condition) {
    if (!emit_expr(ctx, stmt->as.loop_stmt.condition, out, PREC_LOWEST)) return false;
  } else {
    if (fprintf(out, "1") < 0) return false;
  }
  if (fprintf(out, ") {\n") < 0) return false;

  const AstStmt* inner = stmt->as.loop_stmt.body->first;
  while (inner) {
    if (!emit_stmt(ctx, inner, out)) {
      return false;
    }
    inner = inner->next;
  }

  if (stmt->as.loop_stmt.increment) {
    if (fprintf(out, "  ") < 0) return false;
    if (!emit_expr(ctx, stmt->as.loop_stmt.increment, out, PREC_LOWEST)) return false;
    if (fprintf(out, ";\n") < 0) return false;
  }

  if (fprintf(out, "  }\n") < 0) return false;
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
  if (!emit_expr(ctx, stmt->as.match_stmt.subject, out, PREC_LOWEST)) {
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
        if (!emit_expr(ctx, current->pattern, out, PREC_LOWEST)) {
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
    const AstStmt* inner = current->block ? current->block->first : NULL;
    while (inner) {
      if (!emit_stmt(ctx, inner, out)) {
        return false;
      }
      inner = inner->next;
    }
    if (fprintf(out, "  }") < 0) {
      return false;
    }
    case_index += 1;
    current = current->next;
  }
  if (default_case) {
    const AstStmt* inner = default_case->block ? default_case->block->first : NULL;
    if (case_index > 0) {
      if (fprintf(out, " else {\n") < 0) {
        return false;
      }
      while (inner) {
        if (!emit_stmt(ctx, inner, out)) {
          return false;
        }
        inner = inner->next;
      }
      if (fprintf(out, "  }") < 0) {
        return false;
      }
    } else {
      while (inner) {
        if (!emit_stmt(ctx, inner, out)) {
          return false;
        }
        inner = inner->next;
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

