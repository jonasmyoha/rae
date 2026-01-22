/* ast.c - AST helper utilities */

#include "ast.h"

#include <stdio.h>

static void print_indent(FILE* out, int level) {
  for (int i = 0; i < level; ++i) {
    fputs("  ", out);
  }
}

static void print_str(FILE* out, Str s) {
  if (!s.data || s.len == 0) {
    return;
  }
  fprintf(out, "%.*s", (int)s.len, s.data);
}

static void dump_type_ref(const AstTypeRef* type, FILE* out) {
  if (!type) {
    fputs("<type?>", out);
    return;
  }
  if (type->is_opt) fputs("opt ", out);
  if (type->is_view) fputs("view ", out);
  if (type->is_mod) fputs("mod ", out);
  if (type->is_id) fputs("id ", out);
  if (type->is_key) fputs("key ", out);
  
  if (!type->parts) {
    fputs("<base?>", out);
  } else {
    const AstIdentifierPart* part = type->parts;
    bool first = true;
    while (part) {
      if (!first) {
        fputc(' ', out);
      }
      print_str(out, part->text);
      first = false;
      part = part->next;
    }
  }
  if (type->generic_args) {
    fputc('[', out);
    AstTypeRef* generic_param = type->generic_args;
    bool first_generic_param = true;
    while (generic_param) {
      if (!first_generic_param) {
        fputs(", ", out);
      }
      dump_type_ref(generic_param, out);
      first_generic_param = false;
      generic_param = generic_param->next;
    }
    fputc(']', out);
  }
}

static const char* binary_op_name(AstBinaryOp op) {
  switch (op) {
    case AST_BIN_ADD:
      return "+";
    case AST_BIN_SUB:
      return "-";
    case AST_BIN_MUL:
      return "*";
    case AST_BIN_DIV:
      return "/";
    case AST_BIN_MOD:
      return "%";
    case AST_BIN_LT:
      return "<";
    case AST_BIN_GT:
      return ">";
    case AST_BIN_LE:
      return "<=";
    case AST_BIN_GE:
      return ">=";
    case AST_BIN_IS:
      return "is";
    case AST_BIN_AND:
      return "and";
    case AST_BIN_OR:
      return "or";
  }
  return "?";
}

static const char* unary_op_name(AstUnaryOp op) {
  switch (op) {
    case AST_UNARY_NEG:
      return "-";
    case AST_UNARY_NOT:
      return "not";
    case AST_UNARY_SPAWN:
      return "spawn";
    case AST_UNARY_PRE_INC:
      return "++";
    case AST_UNARY_PRE_DEC:
      return "--";
    case AST_UNARY_POST_INC:
      return "++";
    case AST_UNARY_POST_DEC:
      return "--";
    case AST_UNARY_VIEW:
      return "view";
    case AST_UNARY_MOD:
      return "mod";
  }
  return "?";
}

static void dump_expr(const AstExpr* expr, FILE* out);

static void dump_call_args(const AstCallArg* arg, FILE* out) {
  const AstCallArg* current = arg;
  bool first = true;
  while (current) {
    if (!first) {
      fputs(", ", out);
    }
    if (current->name.len > 0) {
      print_str(out, current->name);
      fputs(": ", out);
    }
    dump_expr(current->value, out);
    first = false;
    current = current->next;
  }
}

static void dump_object_fields(const AstObjectField* field, FILE* out) {
  const AstObjectField* current = field;
  bool first = true;
  while (current) {
    if (!first) {
      fputs(", ", out);
    }
    print_str(out, current->name);
    fputs(": ", out);
    dump_expr(current->value, out);
    first = false;
    current = current->next;
  }
}

static void dump_expr(const AstExpr* expr, FILE* out) {
  if (!expr) {
    fputs("<expr?>", out);
    return;
  }
  switch (expr->kind) {
    case AST_EXPR_IDENT:
      print_str(out, expr->as.ident);
      break;
    case AST_EXPR_INTEGER:
      print_str(out, expr->as.integer);
      break;
    case AST_EXPR_FLOAT:
      print_str(out, expr->as.floating);
      break;
    case AST_EXPR_STRING:
      fputc('"', out);
      print_str(out, expr->as.string_lit);
      fputc('"', out);
      break;
    case AST_EXPR_CHAR:
      fputc('\'', out);
      print_str(out, expr->as.char_lit);
      fputc('\'', out);
      break;
    case AST_EXPR_BOOL:
      fputs(expr->as.boolean ? "true" : "false", out);
      break;
    case AST_EXPR_NONE:
      fputs("none", out);
      break;
    case AST_EXPR_BINARY:
      fputc('(', out);
      dump_expr(expr->as.binary.lhs, out);
      fputc(' ', out);
      fputs(binary_op_name(expr->as.binary.op), out);
      fputc(' ', out);
      dump_expr(expr->as.binary.rhs, out);
      fputc(')', out);
      break;
    case AST_EXPR_UNARY:
      fputc('(', out);
      fputs(unary_op_name(expr->as.unary.op), out);
      fputc(' ', out);
      dump_expr(expr->as.unary.operand, out);
      fputc(')', out);
      break;
    case AST_EXPR_CALL:
      dump_expr(expr->as.call.callee, out);
      fputc('(', out);
      dump_call_args(expr->as.call.args, out);
      fputc(')', out);
      break;
    case AST_EXPR_MEMBER:
      dump_expr(expr->as.member.object, out);
      fputc('.', out);
      print_str(out, expr->as.member.member);
      break;
    case AST_EXPR_OBJECT:
      if (expr->as.object_literal.type) {
        dump_type_ref(expr->as.object_literal.type, out);
        fputc(' ', out);
      }
      fputc('{', out);
      dump_object_fields(expr->as.object_literal.fields, out);
      fputc('}', out);
      break;
    case AST_EXPR_MATCH: {
      fputs("match ", out);
      dump_expr(expr->as.match_expr.subject, out);
      fputs(" { ", out);
      AstMatchArm* arm = expr->as.match_expr.arms;
      while (arm) {
        fputs("case ", out);
        if (arm->pattern) {
          dump_expr(arm->pattern, out);
        } else {
          fputs("_", out);
        }
        fputs(" => ", out);
        dump_expr(arm->value, out);
        if (arm->next) {
          fputs(", ", out);
        }
        arm = arm->next;
      }
      fputs(" }", out);
      break;
    }
    case AST_EXPR_LIST: {
      fputc('[', out);
      AstExprList* current = expr->as.list;
      while (current) {
        dump_expr(current->value, out);
        if (current->next) {
          fputs(", ", out);
        }
        current = current->next;
      }
      fputc(']', out);
      break;
    }
    case AST_EXPR_COLLECTION_LITERAL: {
      fputc('{', out);
      AstCollectionElement* current = expr->as.collection.elements;
      bool first = true;
      while (current) {
        if (!first) {
          fputs(", ", out);
        }
        if (current->key) {
          print_str(out, *current->key);
          fputs(": ", out);
        }
        dump_expr(current->value, out);
        first = false;
        current = current->next;
      }
      fputc('}', out);
      break;
    }
    case AST_EXPR_INTERP: {
      fputc('"', out);
      AstInterpPart* part = expr->as.interp.parts;
      while (part) {
        if (part->value->kind == AST_EXPR_STRING) {
          print_str(out, part->value->as.string_lit);
        } else {
          fputs("{", out);
          dump_expr(part->value, out);
          fputs("}", out);
        }
        part = part->next;
      }
      fputc('"', out);
      break;
    }
    case AST_EXPR_INDEX:
      dump_expr(expr->as.index.target, out);
      fputc('[', out);
      dump_expr(expr->as.index.index, out);
      fputc(']', out);
      break;
    case AST_EXPR_METHOD_CALL:
      dump_expr(expr->as.method_call.object, out);
      fputc('.', out);
      print_str(out, expr->as.method_call.method_name);
      fputc('(', out);
      dump_call_args(expr->as.method_call.args, out);
      fputc(')', out);
      break;
  }
}

static void dump_properties(const AstProperty* prop, FILE* out) {
  if (!prop) {
    return;
  }
  fputs(" props(", out);
  const AstProperty* current = prop;
  bool first = true;
  while (current) {
    if (!first) {
      fputs(", ", out);
    }
    print_str(out, current->name);
    first = false;
    current = current->next;
  }
  fputc(')', out);
}

static void dump_block(const AstBlock* block, FILE* out, int indent);

static void dump_def_stmt(const AstStmt* stmt, FILE* out, int indent) {
  print_indent(out, indent);
  fputs("def ", out);
  print_str(out, stmt->as.def_stmt.name);
  fputs(": ", out);
  dump_type_ref(stmt->as.def_stmt.type, out);
  fputs(stmt->as.def_stmt.is_bind ? " => " : " = ", out);
  dump_expr(stmt->as.def_stmt.value, out);
  fputc('\n', out);
}

static void dump_destructure_stmt(const AstStmt* stmt, FILE* out, int indent) {
  print_indent(out, indent);
  fputs("destructure\n", out);
  const AstDestructureBinding* binding = stmt->as.destruct_stmt.bindings;
  while (binding) {
    print_indent(out, indent + 1);
    fputs("binding ", out);
    print_str(out, binding->local_name);
    fputs(" <- ", out);
    print_str(out, binding->return_label);
    fputc('\n', out);
    binding = binding->next;
  }
  print_indent(out, indent + 1);
  fputs("call ", out);
  dump_expr(stmt->as.destruct_stmt.call, out);
  fputc('\n', out);
}

static void dump_expr_stmt(const AstStmt* stmt, FILE* out, int indent) {
  print_indent(out, indent);
  fputs("expr ", out);
  dump_expr(stmt->as.expr_stmt, out);
  fputc('\n', out);
}

static void dump_ret_stmt(const AstStmt* stmt, FILE* out, int indent) {
  print_indent(out, indent);
  fputs("ret", out);
  AstReturnArg* arg = stmt->as.ret_stmt.values;
  bool first = true;
  while (arg) {
    if (first) {
      fputc(' ', out);
    } else {
      fputs(", ", out);
    }
    if (arg->has_label) {
      print_str(out, arg->label);
      fputs(": ", out);
    }
    dump_expr(arg->value, out);
    first = false;
    arg = arg->next;
  }
  fputc('\n', out);
}

static void dump_if_stmt(const AstStmt* stmt, FILE* out, int indent) {
  print_indent(out, indent);
  fputs("if ", out);
  dump_expr(stmt->as.if_stmt.condition, out);
  fputc('\n', out);
  print_indent(out, indent + 1);
  fputs("then\n", out);
  dump_block(stmt->as.if_stmt.then_block, out, indent + 2);
  if (stmt->as.if_stmt.else_block) {
    print_indent(out, indent + 1);
    fputs("else\n", out);
    dump_block(stmt->as.if_stmt.else_block, out, indent + 2);
  }
}

static void dump_loop_stmt(const AstStmt* stmt, FILE* out, int indent) {
  print_indent(out, indent);
  fputs("loop ", out);
  if (stmt->as.loop_stmt.init) {
    if (stmt->as.loop_stmt.init->kind == AST_STMT_DEF) {
        print_str(out, stmt->as.loop_stmt.init->as.def_stmt.name);
        fputs(": ", out);
        dump_type_ref(stmt->as.loop_stmt.init->as.def_stmt.type, out);
        if (stmt->as.loop_stmt.is_range) {
            fputs(" in ", out);
        } else {
             fputs(" = ", out);
             dump_expr(stmt->as.loop_stmt.init->as.def_stmt.value, out);
             fputs(", ", out);
        }
    } else if (stmt->as.loop_stmt.init->kind == AST_STMT_EXPR) {
        dump_expr(stmt->as.loop_stmt.init->as.expr_stmt, out);
        fputs(", ", out);
    }
  }
  
  if (stmt->as.loop_stmt.condition) {
    dump_expr(stmt->as.loop_stmt.condition, out);
  }
  
  if (stmt->as.loop_stmt.increment) {
    fputs(", ", out);
    dump_expr(stmt->as.loop_stmt.increment, out);
  }

  fputc('\n', out);
  dump_block(stmt->as.loop_stmt.body, out, indent + 1);
}

static void dump_assign_stmt(const AstStmt* stmt, FILE* out, int indent) {
  print_indent(out, indent);
  fputs("assign ", out);
  dump_expr(stmt->as.assign_stmt.target, out);
  fputs(stmt->as.assign_stmt.is_bind ? " => " : " = ", out);
  dump_expr(stmt->as.assign_stmt.value, out);
  fputc('\n', out);
}

static void dump_match_stmt(const AstStmt* stmt, FILE* out, int indent) {
  print_indent(out, indent);
  fputs("match ", out);
  dump_expr(stmt->as.match_stmt.subject, out);
  fputc('\n', out);
  AstMatchCase* cases = stmt->as.match_stmt.cases;
  while (cases) {
    print_indent(out, indent + 1);
    if (cases->pattern) {
      fputs("case ", out);
      dump_expr(cases->pattern, out);
    } else {
      fputs("default", out);
    }
    fputc('\n', out);
    dump_block(cases->block, out, indent + 2);
    cases = cases->next;
  }
}

static void dump_block(const AstBlock* block, FILE* out, int indent) {
  if (!block || !block->first) {
    print_indent(out, indent);
    fputs("<empty>\n", out);
    return;
  }
  const AstStmt* stmt = block->first;
  while (stmt) {
    switch (stmt->kind) {
      case AST_STMT_DEF:
        dump_def_stmt(stmt, out, indent);
        break;
      case AST_STMT_DESTRUCT:
        dump_destructure_stmt(stmt, out, indent);
        break;
      case AST_STMT_EXPR:
        dump_expr_stmt(stmt, out, indent);
        break;
      case AST_STMT_RET:
        dump_ret_stmt(stmt, out, indent);
        break;
      case AST_STMT_IF:
        dump_if_stmt(stmt, out, indent);
        break;
      case AST_STMT_LOOP:
        dump_loop_stmt(stmt, out, indent);
        break;
      case AST_STMT_MATCH:
        dump_match_stmt(stmt, out, indent);
        break;
      case AST_STMT_ASSIGN:
        dump_assign_stmt(stmt, out, indent);
        break;
    }
    stmt = stmt->next;
  }
}

static void dump_type_decl(const AstDecl* decl, FILE* out, int indent) {
  print_indent(out, indent);
  fputs("type ", out);
  print_str(out, decl->as.type_decl.name);
  if (decl->as.type_decl.generic_params) {
    fputc('[', out);
    AstIdentifierPart* current = decl->as.type_decl.generic_params;
    bool first = true;
    while (current) {
      if (!first) {
        fputs(", ", out);
      }
      print_str(out, current->text);
      first = false;
      current = current->next;
    }
    fputc(']', out);
  }
  dump_properties(decl->as.type_decl.properties, out);
  fputc('\n', out);
  AstTypeField* field = decl->as.type_decl.fields;
  while (field) {
    print_indent(out, indent + 1);
    fputs("field ", out);
    print_str(out, field->name);
    fputs(": ", out);
    dump_type_ref(field->type, out);
    fputc('\n', out);
    field = field->next;
  }
}

static void dump_return_items(const AstReturnItem* returns, FILE* out, int indent) {
  const AstReturnItem* current = returns;
  while (current) {
    print_indent(out, indent);
    fputs("return ", out);
    if (current->has_name) {
      print_str(out, current->name);
      fputs(": ", out);
    }
    dump_type_ref(current->type, out);
    fputc('\n', out);
    current = current->next;
  }
}

static void dump_params(const AstParam* params, FILE* out, int indent) {
  const AstParam* current = params;
  while (current) {
    print_indent(out, indent);
    fputs("param ", out);
    print_str(out, current->name);
    fputs(": ", out);
    dump_type_ref(current->type, out);
    fputc('\n', out);
    current = current->next;
  }
}

static void dump_enum_decl(const AstDecl* decl, FILE* out, int indent) {
  print_indent(out, indent);
  fputs("enum ", out);
  print_str(out, decl->as.enum_decl.name);
  fputc('\n', out);
  AstEnumMember* current = decl->as.enum_decl.members;
  while (current) {
    print_indent(out, indent + 1);
    print_str(out, current->name);
    fputc('\n', out);
    current = current->next;
  }
}

static void dump_func_decl(const AstDecl* decl, FILE* out, int indent) {
  print_indent(out, indent);
  if (decl->as.func_decl.is_extern) {
    fputs("extern ", out);
  }
  fputs("func ", out);
  print_str(out, decl->as.func_decl.name);
  if (decl->as.func_decl.generic_params) {
    fputc('[', out);
    AstIdentifierPart* current = decl->as.func_decl.generic_params;
    bool first = true;
    while (current) {
      if (!first) {
        fputs(", ", out);
      }
      print_str(out, current->text);
      first = false;
      current = current->next;
    }
    fputc(']', out);
  }
  dump_properties(decl->as.func_decl.properties, out);
  fputc('\n', out);
  dump_params(decl->as.func_decl.params, out, indent + 1);
  dump_return_items(decl->as.func_decl.returns, out, indent + 1);
  if (decl->as.func_decl.body) {
    print_indent(out, indent + 1);
    fputs("body\n", out);
    dump_block(decl->as.func_decl.body, out, indent + 2);
  } else {
    print_indent(out, indent + 1);
    fputs("extern body (none)\n", out);
  }
}

void ast_dump_module(const AstModule* module, FILE* out) {
  if (!module) {
    fputs("<null module>\n", out);
    return;
  }
  fputs("MODULE\n", out);
  const AstDecl* decl = module->decls;
  while (decl) {
    switch (decl->kind) {
      case AST_DECL_TYPE:
        dump_type_decl(decl, out, 1);
        break;
      case AST_DECL_FUNC:
        dump_func_decl(decl, out, 1);
        break;
      case AST_DECL_ENUM:
        dump_enum_decl(decl, out, 1);
        break;
    }
    decl = decl->next;
  }
}
