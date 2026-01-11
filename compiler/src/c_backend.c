#include "c_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lexer.h"

typedef struct {
  const AstParam* params;
  Str locals[256];
  Str local_types[256];
  bool local_is_ptr[256];
  bool local_is_mod[256];
  size_t local_count;
  bool returns_value;
  size_t temp_counter;
} CFuncContext;

// Forward declarations
static bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_function(const AstFuncDecl* func, FILE* out);
static bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_call(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_log_call(CFuncContext* ctx, const AstExpr* expr, FILE* out, bool newline);
static bool emit_call_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_string_literal(FILE* out, Str literal);
static bool emit_param_list(const AstParam* params, FILE* out);
static const char* map_rae_type_to_c(Str type_name);
static bool is_primitive_type(Str type_name);
static bool is_pointer_type(CFuncContext* ctx, Str name);
static bool is_mod_type(CFuncContext* ctx, Str name);
static Str get_local_type_name(CFuncContext* ctx, Str name);
static bool emit_type_ref_as_c_type(const AstTypeRef* type, FILE* out);
static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_loop(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_match(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool is_wildcard_pattern(const AstExpr* expr);
static bool is_string_literal_expr(const AstExpr* expr);
static bool match_cases_use_string(const AstMatchCase* cases, bool* out_use_string);
static bool match_arms_use_string(const AstMatchArm* arms, bool* out_use_string);
static bool func_has_return_value(const AstFuncDecl* func);
static const char* c_return_type(const AstFuncDecl* func);

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
  if (str_eq_cstr(type_name, "Char")) return "int64_t";
  if (str_eq_cstr(type_name, "Float")) return "double";
  if (str_eq_cstr(type_name, "Bool")) return "int8_t";
  if (str_eq_cstr(type_name, "String")) return "const char*";
  if (str_eq_cstr(type_name, "List")) return "RaeList*";
  // For user types, return name. Caller might need to str_to_cstr.
  return NULL; 
}

static bool emit_type_ref_as_c_type(const AstTypeRef* type, FILE* out) {
  if (!type || !type->parts) return fprintf(out, "int64_t") >= 0; // Default to int64_t for unknown types

  const AstIdentifierPart* current = type->parts;
  bool is_ptr = type->is_view || type->is_mod;

  if (type->is_id) {
      return fprintf(out, "int64_t") >= 0;
  }
  if (type->is_key) {
      return fprintf(out, "const char*") >= 0;
  }

  const char* c_type_str = map_rae_type_to_c(current->text);
  if (c_type_str) {
      if (fprintf(out, "%s", c_type_str) < 0) return false;
  } else {
      if (fprintf(out, "%.*s", (int)current->text.len, current->text.data) < 0) return false;
  }
  
  while (current->next) { // Handle multi-part identifiers (e.g., Module.Type)
      current = current->next;
      if (fprintf(out, "_%.*s", (int)current->text.len, current->text.data) < 0) return false; // Concatenate with underscore
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

static bool emit_param_list(const AstParam* params, FILE* out) {
  size_t index = 0;
  const AstParam* param = params;
  if (!param) {
    return fprintf(out, "void") >= 0;
  }
  while (param) {
    if (index > 0) {
      if (fprintf(out, ", ") < 0) return false;
    }
    
    const char* c_type_base = "int64_t";
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
            const char* mapped = map_rae_type_to_c(first);
            if (mapped) c_type_base = mapped;
            else c_type_base = free_me = str_to_cstr(first);
        }
    }

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

static const char* c_return_type(const AstFuncDecl* func) {
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

static bool is_string_expr(const AstExpr* expr) {
  if (!expr) return false;
  if (expr->kind == AST_EXPR_STRING || expr->kind == AST_EXPR_INTERP) return true;
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
  bool is_string = false;
  
  if (is_string_expr(value)) {
    is_string = true;
  } else if (value->kind == AST_EXPR_CALL && value->as.call.callee->kind == AST_EXPR_IDENT) {
    Str name = value->as.call.callee->as.ident;
    if (str_eq_cstr(name, "rae_str") || str_eq_cstr(name, "rae_str_concat")) {
      is_string = true;
    }
  } else if (value->kind == AST_EXPR_IDENT) {
    Str name = value->as.ident;
    // Check params
    for (const AstParam* p = ctx->params; p; p = p->next) {
      if (str_eq(p->name, name)) {
        if (p->type && p->type->parts && str_eq_cstr(p->type->parts->text, "String")) {
          is_string = true;
        }
        break;
      }
    }
    // Check locals
    if (!is_string) {
      for (size_t i = 0; i < ctx->local_count; ++i) {
        if (str_eq(ctx->locals[i], name)) {
          if (str_eq_cstr(ctx->local_types[i], "String")) {
            is_string = true;
          }
          break;
        }
      }
    }
  }

  if (is_string) {
    if (fprintf(out, "  %s(", newline ? "rae_log_cstr" : "rae_log_stream_cstr") < 0) {
      return false;
    }
    if (!emit_expr(ctx, value, out)) return false;
    if (fprintf(out, ");\n") < 0) return false;
    return true;
  }

  // Handle Bool specifically for true/false output
  bool is_bool = false;
  if (value->kind == AST_EXPR_IDENT) {
    Str type_name = get_local_type_name(ctx, value->as.ident);
    if (str_eq_cstr(type_name, "Bool")) is_bool = true;
  } else if (value->kind == AST_EXPR_BOOL) {
    is_bool = true;
  } else if (value->kind == AST_EXPR_BINARY) {
    switch (value->as.binary.op) {
      case AST_BIN_LT: case AST_BIN_GT: case AST_BIN_LE: case AST_BIN_GE:
      case AST_BIN_IS: case AST_BIN_AND: case AST_BIN_OR:
        is_bool = true;
        break;
      default: break;
    }
  } else if (value->kind == AST_EXPR_UNARY) {
    if (value->as.unary.op == AST_UNARY_NOT) is_bool = true;
  }

  if (is_bool) {
    bool val_is_ptr = (value->kind == AST_EXPR_IDENT && is_pointer_type(ctx, value->as.ident));
    if (val_is_ptr) {
        fprintf(out, "  rae_log_stream_cstr(\"%s \");\n", is_mod_type(ctx, value->as.ident) ? "mod" : "view");
    }
    if (fprintf(out, "  %s(", newline ? "rae_log_bool" : "rae_log_stream_bool") < 0) return false;
    if (val_is_ptr) fprintf(out, "(*");
    if (!emit_expr(ctx, value, out)) return false;
    if (val_is_ptr) fprintf(out, ")");
    if (fprintf(out, ");\n") < 0) return false;
    return true;
  }

  // Handle Char specifically
  bool is_char = false;
  if (value->kind == AST_EXPR_IDENT) {
    Str type_name = get_local_type_name(ctx, value->as.ident);
    if (str_eq_cstr(type_name, "Char")) is_char = true;
  } else if (value->kind == AST_EXPR_CHAR) {
    is_char = true;
  }

  if (is_char) {
    bool val_is_ptr = (value->kind == AST_EXPR_IDENT && is_pointer_type(ctx, value->as.ident));
    if (val_is_ptr) {
        fprintf(out, "  rae_log_stream_cstr(\"%s \");\n", is_mod_type(ctx, value->as.ident) ? "mod" : "view");
    }
    if (fprintf(out, "  %s(", newline ? "rae_log_char" : "rae_log_stream_char") < 0) return false;
    if (val_is_ptr) fprintf(out, "(*");
    if (!emit_expr(ctx, value, out)) return false;
    if (val_is_ptr) fprintf(out, ")");
    if (fprintf(out, ");\n") < 0) return false;
    return true;
  }

  // Handle Id/Key specifically
  bool is_id = false;
  bool is_key = false;
  if (value->kind == AST_EXPR_IDENT) {
    Str name = value->as.ident;
    // Check params
    for (const AstParam* p = ctx->params; p; p = p->next) {
      if (str_eq(p->name, name)) {
        if (p->type && p->type->is_id) is_id = true;
        if (p->type && p->type->is_key) is_key = true;
        break;
      }
    }
    // Check locals
    if (!is_id && !is_key) {
      for (size_t i = 0; i < ctx->local_count; ++i) {
        if (str_eq(ctx->locals[i], name)) {
          // Need to check if local was id/key... 
          // I didn't store is_id/is_key in CFuncContext yet.
          // Let's check type name fallback.
          if (str_eq_cstr(ctx->local_types[i], "id")) is_id = true;
          if (str_eq_cstr(ctx->local_types[i], "key")) is_key = true;
          break;
        }
      }
    }
  }

  if (is_id) {
    if (fprintf(out, "  %s(", newline ? "rae_log_id" : "rae_log_stream_id") < 0) return false;
    if (!emit_expr(ctx, value, out)) return false;
    if (fprintf(out, ");\n") < 0) return false;
    return true;
  }
  if (is_key) {
    if (fprintf(out, "  %s(", newline ? "rae_log_key" : "rae_log_stream_key") < 0) return false;
    if (!emit_expr(ctx, value, out)) return false;
    if (fprintf(out, ");\n") < 0) return false;
    return true;
  }

  bool is_float = false;
  if (value->kind == AST_EXPR_FLOAT) {
    is_float = true;
  } else if (value->kind == AST_EXPR_IDENT) {
    Str name = value->as.ident;
    for (const AstParam* p = ctx->params; p; p = p->next) {
      if (str_eq(p->name, name)) {
        if (p->type && p->type->parts && str_eq_cstr(p->type->parts->text, "Float")) {
          is_float = true;
        }
        break;
      }
    }
    if (!is_float) {
      for (size_t i = 0; i < ctx->local_count; ++i) {
        if (str_eq(ctx->locals[i], name)) {
          if (str_eq_cstr(ctx->local_types[i], "Float")) {
            is_float = true;
          }
          break;
        }
      }
    }
  } else if (value->kind == AST_EXPR_CALL) {
      // HACK: assume some calls return float? No type info here easily.
      // But random() is a call.
      if (value->as.call.callee->kind == AST_EXPR_IDENT && str_eq_cstr(value->as.call.callee->as.ident, "random")) {
          is_float = true;
      }
  }

  if (is_float) {
    bool val_is_ptr = (value->kind == AST_EXPR_IDENT && is_pointer_type(ctx, value->as.ident));
    if (val_is_ptr) {
        fprintf(out, "  rae_log_stream_cstr(\"%s \");\n", is_mod_type(ctx, value->as.ident) ? "mod" : "view");
    }
    if (fprintf(out, "  %s(", newline ? "rae_log_float" : "rae_log_stream_float") < 0) {
      return false;
    }
    if (val_is_ptr) fprintf(out, "(*");
    if (!emit_expr(ctx, value, out)) return false;
    if (val_is_ptr) fprintf(out, ")");
    if (fprintf(out, ");\n") < 0) return false;
    return true;
  }

  bool is_list = false;
  if (value->kind == AST_EXPR_IDENT) {
    Str name = value->as.ident;
    for (const AstParam* p = ctx->params; p; p = p->next) {
      if (str_eq(p->name, name)) {
        if (p->type && p->type->parts && str_eq_cstr(p->type->parts->text, "List")) {
          is_list = true;
        }
        break;
      }
    }
    if (!is_list) {
      for (size_t i = 0; i < ctx->local_count; ++i) {
        if (str_eq(ctx->locals[i], name)) {
          if (str_eq_cstr(ctx->local_types[i], "List")) {
            is_list = true;
          }
          break;
        }
      }
    }
  } else if (value->kind == AST_EXPR_COLLECTION_LITERAL || value->kind == AST_EXPR_LIST) {
      is_list = true;
  }

  if (is_list) {
    if (fprintf(out, "  %s(", newline ? "rae_log_list" : "rae_log_stream_list") < 0) {
      return false;
    }
    if (!emit_expr(ctx, value, out)) return false;
    if (fprintf(out, ");\n") < 0) return false;
    return true;
  }
  
  bool val_is_ptr = (value->kind == AST_EXPR_IDENT && is_pointer_type(ctx, value->as.ident));
  if (val_is_ptr) {
      fprintf(out, "  rae_log_stream_cstr(\"%s \");\n", is_mod_type(ctx, value->as.ident) ? "mod" : "view");
  }
  if (fprintf(out, "  %s(", newline ? "rae_log_i64" : "rae_log_stream_i64") < 0) {
    return false;
  }
  if (val_is_ptr) fprintf(out, "(*");
  if (!emit_expr(ctx, value, out)) {
    return false;
  }
  if (val_is_ptr) fprintf(out, ")");
  if (fprintf(out, ");\n") < 0) {
    return false;
  }
  return true;
}

static bool is_primitive_type(Str type_name) {
  return str_eq_cstr(type_name, "Int") || 
         str_eq_cstr(type_name, "Float") || 
         str_eq_cstr(type_name, "Bool") || 
         str_eq_cstr(type_name, "Char") ||
         str_eq_cstr(type_name, "String") ||
         str_eq_cstr(type_name, "List") ||
         str_eq_cstr(type_name, "Array");
}

static bool emit_call_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
  if (!expr || expr->kind != AST_EXPR_CALL || !expr->as.call.callee ||
      expr->as.call.callee->kind != AST_EXPR_IDENT) {
    fprintf(stderr, "error: only direct function calls are supported in expressions\n");
    return false;
  }
  Str name = expr->as.call.callee->as.ident;
  if (str_eq_cstr(name, "log") || str_eq_cstr(name, "logS")) {
    fprintf(stderr, "error: log/logS cannot be used as expressions\n");
    return false;
  }
  if (fprintf(out, "%.*s(", (int)name.len, name.data) < 0) {
    return false;
  }
  const AstCallArg* arg = expr->as.call.args;
  size_t arg_index = 0;
  while (arg) {
    if (arg_index > 0) {
      if (fprintf(out, ", ") < 0) return false;
    }
    
    // HACK: If the argument is a local struct variable, we might need to pass it by address
    // if the target function expects a pointer. Since we don't have type info for the target
    // here easily, we'll try a heuristic: if it's a local struct, pass by address.
    bool needs_addr = false;
    if (arg->value->kind == AST_EXPR_IDENT) {
        for (size_t i = 0; i < ctx->local_count; i++) {
            if (str_eq(ctx->locals[i], arg->value->as.ident)) {
                // It's a local. Is it a struct?
                if (!is_primitive_type(ctx->local_types[i])) {
                    needs_addr = true;
                }
                break;
            }
        }
        if (needs_addr && is_pointer_type(ctx, arg->value->as.ident)) {
            needs_addr = false;
        }
    } else if (arg->value->kind == AST_EXPR_UNARY && (arg->value->as.unary.op == AST_UNARY_VIEW || arg->value->as.unary.op == AST_UNARY_MOD)) {
        // If it's explicitly view/mod, we don't need another &
        needs_addr = false;
    }

    if (needs_addr) {
        if (fprintf(out, "&") < 0) return false;
    }

    if (!emit_expr(ctx, arg->value, out)) {
      return false;
    }
    arg = arg->next;
    arg_index += 1;
  }
  return fprintf(out, ")") >= 0;
}

static bool is_pointer_type(CFuncContext* ctx, Str name) {
  for (const AstParam* param = ctx->params; param; param = param->next) {
    if (str_eq(param->name, name)) {
        if (param->type) {
            return param->type->is_view || param->type->is_mod;
        }
    }
  }
  for (size_t i = 0; i < ctx->local_count; ++i) {
    if (str_eq(ctx->locals[i], name)) {
      return ctx->local_is_ptr[i];
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

static bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
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
          fprintf(out, "rae_str_concat(");
      }
      
      // Initial part
      if (!emit_expr(ctx, part->value, out)) return false;
      part = part->next;
      
      while (part) {
          fprintf(out, ", ");
          if (part->value->kind != AST_EXPR_STRING) {
              fprintf(out, "rae_str(");
              if (!emit_expr(ctx, part->value, out)) return false;
              fprintf(out, ")");
          } else {
              if (!emit_expr(ctx, part->value, out)) return false;
          }
          fprintf(out, ")"); // close rae_str_concat
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
      fprintf(stderr,
              "error: C backend currently only supports referencing parameters or prior locals "
              "(%.*s)\n",
              (int)expr->as.ident.len, expr->as.ident.data);
      return false;
    }
    case AST_EXPR_BINARY: {
      const char* op = NULL;
      switch (expr->as.binary.op) {
        case AST_BIN_ADD:
          op = "+";
          break;
        case AST_BIN_SUB:
          op = "-";
          break;
        case AST_BIN_MUL:
          op = "*";
          break;
        case AST_BIN_DIV:
          op = "/";
          break;
        case AST_BIN_MOD:
          op = "%";
          break;
        case AST_BIN_LT:
          op = "<";
          break;
        case AST_BIN_GT:
          op = ">";
          break;
        case AST_BIN_LE:
          op = "<=";
          break;
        case AST_BIN_GE:
          op = ">=";
          break;
        case AST_BIN_IS:
          op = "==";
          break;
        case AST_BIN_AND:
          op = "&&";
          break;
        case AST_BIN_OR:
          op = "||";
          break;
        default:
          fprintf(stderr, "error: C backend does not support this binary operator yet\n");
          return false;
      }
      if (fprintf(out, "(") < 0) return false;
      bool lhs_is_ptr = (expr->as.binary.lhs->kind == AST_EXPR_IDENT && is_pointer_type(ctx, expr->as.binary.lhs->as.ident));
      if (lhs_is_ptr) fprintf(out, "(*");
      if (!emit_expr(ctx, expr->as.binary.lhs, out)) return false;
      if (lhs_is_ptr) fprintf(out, ")");
      
      if (fprintf(out, " %s ", op) < 0) return false;
      
      bool rhs_is_ptr = (expr->as.binary.rhs->kind == AST_EXPR_IDENT && is_pointer_type(ctx, expr->as.binary.rhs->as.ident));
      if (rhs_is_ptr) fprintf(out, "(*");
      if (!emit_expr(ctx, expr->as.binary.rhs, out)) return false;
      if (rhs_is_ptr) fprintf(out, ")");
      if (fprintf(out, ")") < 0) return false;
      return true;
    }
    case AST_EXPR_UNARY:
      if (expr->as.unary.op == AST_UNARY_NEG) {
        if (fprintf(out, "(-") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out)) return false;
        return fprintf(out, ")") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_NOT) {
        if (fprintf(out, "(!(") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out)) return false;
        return fprintf(out, "))") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_PRE_INC) {
        if (fprintf(out, "++") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out)) return false;
        return true;
      } else if (expr->as.unary.op == AST_UNARY_PRE_DEC) {
        if (fprintf(out, "--") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out)) return false;
        return true;
      } else if (expr->as.unary.op == AST_UNARY_POST_INC) {
        if (!emit_expr(ctx, expr->as.unary.operand, out)) return false;
        return fprintf(out, "++") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_POST_DEC) {
        if (!emit_expr(ctx, expr->as.unary.operand, out)) return false;
        return fprintf(out, "--") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_VIEW || expr->as.unary.op == AST_UNARY_MOD) {
        // In C backend, references are pointers. We take the address of the operand.
        if (fprintf(out, "(&(") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out)) return false;
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
      if (!emit_expr(ctx, expr->as.match_expr.subject, out)) {
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
          if (!emit_expr(ctx, arm->value, out)) {
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
          if (!emit_expr(ctx, arm->pattern, out)) {
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
          if (!emit_expr(ctx, arm->value, out)) {
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
      const char* sep = ".";
      if (expr->as.member.object->kind == AST_EXPR_IDENT) {
          if (is_pointer_type(ctx, expr->as.member.object->as.ident)) {
              sep = "->";
          }
      }
      
      if (!emit_expr(ctx, expr->as.member.object, out)) return false;
      return fprintf(out, "%s%.*s", sep, (int)expr->as.member.member.len, expr->as.member.member.data) >= 0;
    }
    case AST_EXPR_OBJECT: {
      if (expr->as.object_literal.type) {
        fprintf(out, "(");
        if (!emit_type_ref_as_c_type(expr->as.object_literal.type, out)) return false;
        fprintf(out, ")");
      }
      if (fprintf(out, "{ ") < 0) return false;
      const AstObjectField* field = expr->as.object_literal.fields;
      while (field) {
        if (fprintf(out, ".%.*s = ", (int)field->name.len, field->name.data) < 0) return false;
        if (!emit_expr(ctx, field->value, out)) return false;
        field = field->next;
        if (field) {
          if (fprintf(out, ", ") < 0) return false;
        }
      }
      return fprintf(out, " }") >= 0;
    }
    case AST_EXPR_LIST: {
      uint16_t element_count = 0;
      const AstExprList* current = expr->as.list;
      while (current) { element_count++; current = current->next; }
      
      fprintf(out, "__extension__ ({ RaeList* _l = rae_list_create(%u); ", element_count);
      current = expr->as.list;
      while (current) {
        fprintf(out, "rae_list_add(_l, ");
        if (!emit_expr(ctx, current->value, out)) return false;
        fprintf(out, "); ");
        current = current->next;
      }
      fprintf(out, "_l; })");
      return true;
    }
    case AST_EXPR_INDEX: {
      fprintf(out, "rae_list_get(");
      if (!emit_expr(ctx, expr->as.index.target, out)) return false;
      fprintf(out, ", ");
      if (!emit_expr(ctx, expr->as.index.index, out)) return false;
      fprintf(out, ")");
      return true;
    }
    case AST_EXPR_METHOD_CALL: {
      Str method = expr->as.method_call.method_name;
      const char* c_func = NULL;
      char* free_me = NULL;
      if (str_eq_cstr(method, "length")) c_func = "rae_list_length";
      else if (str_eq_cstr(method, "add")) c_func = "rae_list_add";
      else if (str_eq_cstr(method, "get")) c_func = "rae_list_get";
      else c_func = free_me = str_to_cstr(method);
      
      if (c_func) {
        fprintf(out, "%s(", c_func);
        
        // Emit receiver
        const AstExpr* receiver = expr->as.method_call.object;
        bool needs_addr = false;
        if (receiver->kind == AST_EXPR_IDENT) {
            Str type_name = get_local_type_name(ctx, receiver->as.ident);
            if (type_name.len > 0 && !is_primitive_type(type_name) && !is_pointer_type(ctx, receiver->as.ident)) {
                needs_addr = true;
            }
        }
        
        if (needs_addr) fprintf(out, "&(");
        if (!emit_expr(ctx, receiver, out)) { if(free_me) free(free_me); return false; }
        if (needs_addr) fprintf(out, ")");

        const AstCallArg* arg = expr->as.method_call.args;
        while (arg) {
          fprintf(out, ", ");
          if (!emit_expr(ctx, arg->value, out)) { if(free_me) free(free_me); return false; }
          arg = arg->next;
        }
        fprintf(out, ")");
        if (free_me) free(free_me);
        return true;
      }
      fprintf(stderr, "warning: C backend fallback for method call '%.*s'\n", (int)method.len, method.data);
      return fprintf(out, "NULL /* Unsupported method call */") >= 0;
    }
    case AST_EXPR_COLLECTION_LITERAL: {
      uint16_t element_count = 0;
      const AstCollectionElement* current = expr->as.collection.elements;
      while (current) { element_count++; current = current->next; }
      
      bool is_object = (expr->as.collection.elements && expr->as.collection.elements->key != NULL);
      
      if (is_object) {
        if (expr->as.collection.type) {
          fprintf(out, "(");
          if (!emit_type_ref_as_c_type(expr->as.collection.type, out)) return false;
          fprintf(out, ")");
        }
        fprintf(out, "{ ");
        current = expr->as.collection.elements;
        while (current) {
          if (current->key) {
            fprintf(out, ".%.*s = ", (int)current->key->len, current->key->data);
          }
          if (!emit_expr(ctx, current->value, out)) return false;
          if (current->next) fprintf(out, ", ");
          current = current->next;
        }
        fprintf(out, " }");
        return true;
      }
      
      fprintf(out, "__extension__ ({ RaeList* _l = rae_list_create(%u); ", element_count);
      current = expr->as.collection.elements;
      while (current) {
        fprintf(out, "rae_list_add(_l, ");
        if (!emit_expr(ctx, current->value, out)) return false;
        fprintf(out, "); ");
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
      if (!emit_expr(ctx, expr, out)) return false;
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
  if (!emit_expr(ctx, expr, out)) {
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
      if (!emit_expr(ctx, arg->value, out)) {
        return false;
      }
      return fprintf(out, ";\n") >= 0;
    }
        case AST_STMT_DEF: {
          if (!stmt->as.def_stmt.value) {
            fprintf(stderr, "error: C backend def statements require an initializer\n");
            return false;
          }
          if (ctx->local_count >= sizeof(ctx->locals) / sizeof(ctx->locals[0])) {
            fprintf(stderr, "error: C backend local limit exceeded\n");
            return false;
          }
    
          fprintf(out, "  ");
          if (stmt->as.def_stmt.type) {
              if (!emit_type_ref_as_c_type(stmt->as.def_stmt.type, out)) return false;
          } else {
              fprintf(out, "int64_t");
          }
          
                fprintf(out, " %.*s = ", (int)stmt->as.def_stmt.name.len, stmt->as.def_stmt.name.data);
          
                            bool val_needs_addr = stmt->as.def_stmt.is_bind;
                            if (val_needs_addr) {
                                // If RHS is a call, it already returns a pointer (reference)
                                if (stmt->as.def_stmt.value->kind == AST_EXPR_CALL || stmt->as.def_stmt.value->kind == AST_EXPR_METHOD_CALL ||
                                    stmt->as.def_stmt.value->kind == AST_EXPR_NONE) {
                                    val_needs_addr = false;
                                }
                                // If RHS is an identifier that is already a pointer
                                else if (stmt->as.def_stmt.value->kind == AST_EXPR_IDENT) {                              if (is_pointer_type(ctx, stmt->as.def_stmt.value->as.ident)) {
                                  val_needs_addr = false;
                              }
                          }
                      }
                      
                      if (val_needs_addr) {
                          if (fprintf(out, "&(") < 0) return false;
                      }
                
                      if (!emit_expr(ctx, stmt->as.def_stmt.value, out)) {
                        return false;
                      }
                      
                      if (val_needs_addr) {
                          if (fprintf(out, ")") < 0) return false;
                      }          
                if (fprintf(out, ";\n") < 0) {
                  return false;
                }          
                      Str base_type_name = str_from_cstr("Int");
                      bool is_ptr = false;
                      bool is_mod = false;
                      if (stmt->as.def_stmt.type) {
                          is_ptr = stmt->as.def_stmt.type->is_view || stmt->as.def_stmt.type->is_mod;
                          is_mod = stmt->as.def_stmt.type->is_mod;
                          if (stmt->as.def_stmt.type->is_id) {
                              base_type_name = str_from_cstr("id");
                          } else if (stmt->as.def_stmt.type->is_key) {
                              base_type_name = str_from_cstr("key");
                          } else if (stmt->as.def_stmt.type->parts) {
                              base_type_name = stmt->as.def_stmt.type->parts->text;
                          }
                      }          
                ctx->locals[ctx->local_count] = stmt->as.def_stmt.name;
                ctx->local_types[ctx->local_count] = base_type_name;
                ctx->local_is_ptr[ctx->local_count] = is_ptr;
                ctx->local_is_mod[ctx->local_count] = is_mod;
                ctx->local_count++;          return true;
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
          fprintf(out, "(*%.*s)", (int)stmt->as.assign_stmt.target->as.ident.len, stmt->as.assign_stmt.target->as.ident.data);
      } else {
          if (!emit_expr(ctx, stmt->as.assign_stmt.target, out)) return false;
      }
      
      if (fprintf(out, " = ") < 0) return false;
      
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
      if (!emit_expr(ctx, stmt->as.assign_stmt.value, out)) return false;
      if (val_needs_addr) {
          if (fprintf(out, ")") < 0) return false;
      }
      return fprintf(out, ";\n") >= 0;
    }
    default:
      fprintf(stderr, "error: C backend does not yet support this statement kind (%d)\n",
              (int)stmt->kind);
      return false;
  }
}

static bool emit_function(const AstFuncDecl* func, FILE* out) {
  if (func->is_extern) {
    return true;
  }
  if (!func->body) {
    fprintf(stderr, "error: C backend requires function bodies to be present\n");
    return false;
  }
  bool is_main = str_eq_cstr(func->name, "main");
  const char* return_type = c_return_type(func);
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
    ok = fprintf(out, "RAE_UNUSED static %s %s(", return_type, name) >= 0;
  }
  if (ok) {
    ok = emit_param_list(func->params, out);
  }
  if (ok) {
    ok = fprintf(out, ") {\n") >= 0;
  }
  if (!ok) {
    free(name);
    return false;
  }
  CFuncContext ctx = {.params = func->params,
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

static bool emit_struct_defs(const AstModule* module, FILE* out) {
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_TYPE) {
      char* name = str_to_cstr(decl->as.type_decl.name);
      if (fprintf(out, "typedef struct {\n") < 0) { free(name); return false; }
      const AstTypeField* field = decl->as.type_decl.fields;
      while (field) {
        const char* c_type = "int64_t";
        if (field->type && field->type->parts) {
            const char* mapped = map_rae_type_to_c(field->type->parts->text);
            if (mapped) c_type = mapped;
            else c_type = str_to_cstr(field->type->parts->text); // leak
        }
        if (fprintf(out, "  %s %.*s;\n", c_type, (int)field->name.len, field->name.data) < 0) {
          free(name); return false;
        }
        field = field->next;
      }
      if (fprintf(out, "} %s;\n\n", name) < 0) { free(name); return false; }
      free(name);
    }
  }
  return true;
}

bool c_backend_emit(const AstModule* module, const char* out_path) {
  if (!module || !out_path) return false;
  FILE* out = fopen(out_path, "w");
  if (!out) {
    fprintf(stderr, "error: unable to open '%s' for C backend output\n", out_path);
    return false;
  }
  bool ok = true;
  if (fprintf(out,
              "/* Generated by Rae C backend (experimental) */\n#include <stdint.h>\n#include "
              "<string.h>\n#include \"rae_runtime.h\"\n\n") < 0) {
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
    if (str_eq_cstr(fn->name, "main")) {
      has_main = true;
      continue;
    }
    char* proto = str_to_cstr(fn->name);
    if (!proto) {
      ok = false;
      break;
    }
    const char* return_type = c_return_type(fn);
    if (!return_type) {
      free(proto);
      ok = false;
      break;
    }
    const char* qualifier = fn->is_extern ? "extern" : "RAE_UNUSED static";
    if (fprintf(out, "%s %s %s(", qualifier, return_type, proto) < 0) {
      free(proto);
      ok = false;
      break;
    }
    if (!emit_param_list(fn->params, out)) {
      free(proto);
      ok = false;
      break;
    }
    if (fprintf(out, ");\n") < 0) {
      free(proto);
      ok = false;
      break;
    }
    free(proto);
  }
  if (ok && func_count > 1) {
    ok = fprintf(out, "\n") >= 0;
  }
  for (size_t i = 0; ok && i < func_count; ++i) {
    if (funcs[i]->is_extern) {
      continue;
    }
    ok = emit_function(funcs[i], out);
  }
  if (ok && !has_main) {
    fprintf(stderr, "error: C backend could find `func main`\n");
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
  if (!emit_expr(ctx, stmt->as.if_stmt.condition, out)) {
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
    if (!emit_expr(ctx, stmt->as.loop_stmt.condition, out)) return false;
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
    if (!emit_expr(ctx, stmt->as.loop_stmt.increment, out)) return false;
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
  if (use_string) {
    if (fprintf(out, "  const char* __match%zu = ", temp_id) < 0) return false;
  } else if (is_ptr) {
    if (fprintf(out, "  void* __match%zu = ", temp_id) < 0) return false;
  } else {
    if (fprintf(out, "  int64_t __match%zu = ", temp_id) < 0) return false;
  }
  if (!emit_expr(ctx, stmt->as.match_stmt.subject, out)) {
    return false;
  }
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
    } else {
      if (fprintf(out, "%s(__match%zu == ", case_index > 0 ? " else if " : "  if ", temp_id) < 0) {
        return false;
      }
    }
    if (!emit_expr(ctx, current->pattern, out)) {
      return false;
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

