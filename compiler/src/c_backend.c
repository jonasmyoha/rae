#include "c_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
  const AstParam* params;
  Str locals[256];
  size_t local_count;
  bool returns_value;
  size_t temp_counter;
} CFuncContext;

static bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_function(const AstFuncDecl* func, FILE* out);
static bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_call(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_log_call(CFuncContext* ctx, const AstExpr* expr, FILE* out, bool newline);
static bool emit_call_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_string_literal(FILE* out, Str literal);
static bool emit_param_list(const AstParam* params, FILE* out);
static bool type_is_string(const AstTypeRef* type);
static const char* c_return_type(const AstFuncDecl* func);
static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_while(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_match(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool is_wildcard_pattern(const AstExpr* expr);
static bool is_string_literal(const AstExpr* expr);
static bool match_cases_use_string(const AstMatchCase* cases, bool* out_use_string);
static bool match_arms_use_string(const AstMatchArm* arms, bool* out_use_string);

static bool emit_string_literal(FILE* out, Str literal) {
  if (!literal.data) return false;
  return fprintf(out, "%.*s", (int)literal.len, literal.data) >= 0;
}

static bool type_is_string(const AstTypeRef* type) {
  if (!type || !type->parts) {
    return false;
  }
  if (type->parts->next) {
    return false;
  }
  return str_eq_cstr(type->parts->text, "String");
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
    const char* c_type = type_is_string(param->type) ? "const char*" : "int64_t";
    if (fprintf(out, "%s %.*s", c_type, (int)param->name.len, param->name.data) < 0) {
      return false;
    }
    param = param->next;
    index += 1;
  }
  return true;
}

static const char* c_return_type(const AstFuncDecl* func) {
  if (!func->returns) {
    return "void";
  }
  if (func->returns->next) {
    fprintf(stderr, "error: C backend only supports single return values per function\n");
    return NULL;
  }
  if (type_is_string(func->returns->type)) {
    if (!func->is_extern) {
      fprintf(stderr, "error: only extern functions may return String currently\n");
      return NULL;
    }
    return "const char*";
  }
  return "int64_t";
}

static bool emit_log_call(CFuncContext* ctx, const AstExpr* expr, FILE* out, bool newline) {
  const AstCallArg* arg = expr->as.call.args;
  if (!arg || arg->next) {
    fprintf(stderr,
            "error: C backend expects exactly one argument for log/logS during codegen\n");
    return false;
  }
  const AstExpr* value = arg->value;
  if (value->kind == AST_EXPR_STRING) {
    if (fprintf(out, "  %s(", newline ? "rae_log_cstr" : "rae_log_stream_cstr") < 0) {
      return false;
    }
    if (!emit_string_literal(out, value->as.string_lit)) {
      return false;
    }
    if (fprintf(out, ");\n") < 0) {
      return false;
    }
    return true;
  }
  if (fprintf(out, "  %s(", newline ? "rae_log_i64" : "rae_log_stream_i64") < 0) {
    return false;
  }
  if (!emit_expr(ctx, value, out)) {
    return false;
  }
  if (fprintf(out, ");\n") < 0) {
    return false;
  }
  return true;
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
    if (!emit_expr(ctx, arg->value, out)) {
      return false;
    }
    arg = arg->next;
    arg_index += 1;
  }
  return fprintf(out, ")") >= 0;
}

static bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
  if (!expr) return false;
  switch (expr->kind) {
    case AST_EXPR_STRING:
      return emit_string_literal(out, expr->as.string_lit);
    case AST_EXPR_INTEGER:
      return fprintf(out, "%.*s", (int)expr->as.integer.len, expr->as.integer.data) >= 0;
    case AST_EXPR_BOOL:
      return fprintf(out, "%d", expr->as.boolean ? 1 : 0) >= 0;
    case AST_EXPR_IDENT: {
      for (const AstParam* param = ctx->params; param; param = param->next) {
        if (str_eq(param->name, expr->as.ident)) {
          return fprintf(out, "%.*s", (int)expr->as.ident.len, expr->as.ident.data) >= 0;
        }
      }
      for (size_t i = 0; i < ctx->local_count; ++i) {
        if (str_eq(ctx->locals[i], expr->as.ident)) {
          return fprintf(out, "%.*s", (int)expr->as.ident.len, expr->as.ident.data) >= 0;
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
      if (!emit_expr(ctx, expr->as.binary.lhs, out)) return false;
      if (fprintf(out, " %s ", op) < 0) return false;
      if (!emit_expr(ctx, expr->as.binary.rhs, out)) return false;
      return fprintf(out, ")") >= 0;
    }
    case AST_EXPR_UNARY:
      if (expr->as.unary.op == AST_UNARY_NEG) {
        if (fprintf(out, "(-") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out)) return false;
        return fprintf(out, ")") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_NOT) {
        if (fprintf(out, "(!") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out)) return false;
        return fprintf(out, ")") >= 0;
      }
      fprintf(stderr, "error: C backend unsupported unary operator\n");
      return false;
    case AST_EXPR_MATCH: {
      bool use_string = false;
      if (!match_arms_use_string(expr->as.match_expr.arms, &use_string)) {
        return false;
      }
      size_t temp_id = ctx->temp_counter++;
      if (fprintf(out, "({ %s __match%zu = ", use_string ? "const char*" : "int64_t", temp_id) < 0) {
        return false;
      }
      if (!emit_expr(ctx, expr->as.match_expr.subject, out)) {
        return false;
      }
      if (fprintf(out, "; int64_t __result%zu; ", temp_id) < 0) {
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
  }
  fprintf(stderr, "error: unsupported expression kind in C backend\n");
  return false;
}

static bool emit_call(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
  if (expr->kind != AST_EXPR_CALL || !expr->as.call.callee ||
      expr->as.call.callee->kind != AST_EXPR_IDENT) {
    fprintf(stderr, "error: unsupported expression in C backend (only direct calls allowed)\n");
    return false;
  }
  Str name = expr->as.call.callee->as.ident;
  if (str_eq_cstr(name, "log")) {
    return emit_log_call(ctx, expr, out, true);
  }
  if (str_eq_cstr(name, "logS")) {
    return emit_log_call(ctx, expr, out, false);
  }
  if (fprintf(out, "  ") < 0) return false;
  if (!emit_call_expr(ctx, expr, out)) {
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
      if (fprintf(out, "  int64_t %.*s = ", (int)stmt->as.def_stmt.name.len,
                  stmt->as.def_stmt.name.data) < 0) {
        return false;
      }
      if (!emit_expr(ctx, stmt->as.def_stmt.value, out)) {
        return false;
      }
      if (fprintf(out, ";\n") < 0) {
        return false;
      }
      ctx->locals[ctx->local_count++] = stmt->as.def_stmt.name;
      return true;
    }
    case AST_STMT_EXPR:
      return emit_call(ctx, stmt->as.expr_stmt, out);
    case AST_STMT_IF:
      return emit_if(ctx, stmt, out);
    case AST_STMT_WHILE:
      return emit_while(ctx, stmt, out);
    case AST_STMT_MATCH:
      return emit_match(ctx, stmt, out);
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
    ok = fprintf(out, "static %s %s(", return_type, name) >= 0;
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
    const char* qualifier = fn->is_extern ? "extern" : "static";
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
    fprintf(stderr, "error: C backend could not find `func main`\n");
    ok = false;
  }
  free(funcs);
  fclose(out);
  return ok;
}
static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_while(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_match(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool is_wildcard_pattern(const AstExpr* expr);

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

static bool emit_while(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  if (!stmt->as.while_stmt.condition || !stmt->as.while_stmt.body) {
    fprintf(stderr, "error: C backend requires while condition and body\n");
    return false;
  }
  if (fprintf(out, "  while (") < 0) {
    return false;
  }
  if (!emit_expr(ctx, stmt->as.while_stmt.condition, out)) {
    return false;
  }
  if (fprintf(out, ") {\n") < 0) {
    return false;
  }
  const AstStmt* inner = stmt->as.while_stmt.body->first;
  while (inner) {
    if (!emit_stmt(ctx, inner, out)) {
      return false;
    }
    inner = inner->next;
  }
  if (fprintf(out, "  }\n") < 0) {
    return false;
  }
  return true;
}

static bool is_wildcard_pattern(const AstExpr* expr) {
  return expr && expr->kind == AST_EXPR_IDENT && str_eq_cstr(expr->as.ident, "_");
}

static bool is_string_literal(const AstExpr* expr) {
  return expr && expr->kind == AST_EXPR_STRING;
}

static bool match_cases_use_string(const AstMatchCase* cases, bool* out_use_string) {
  bool saw_string = false;
  bool saw_other = false;
  while (cases) {
    if (cases->pattern) {
      if (is_string_literal(cases->pattern)) {
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
      if (is_string_literal(arms->pattern)) {
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
  size_t temp_id = ctx->temp_counter++;
  if (use_string) {
    if (fprintf(out, "  const char* __match%zu = ", temp_id) < 0) return false;
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
