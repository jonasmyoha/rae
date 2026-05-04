// vm_emit_expr.c — Bytecode emission for Rae expressions.
//
// `compile_expr` walks the AST and emits the VM instructions for each
// expression node. Method calls and regular calls are dispatched via
// `compile_call` (in vm_compiler.c).

#include "vm_compiler_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "diag.h"
#include "sema.h"
#include "str.h"
#include "vm.h"
#include "vm_chunk.h"
#include "vm_value.h"

bool compile_expr(BytecodeCompiler* compiler, const AstExpr* expr) {
  if (!expr) {
    return false;
  }
  switch (expr->kind) {
    case AST_EXPR_IDENT: {
      Str name = expr->as.ident;
      int local = compiler_find_local(compiler, name);
      if (local >= 0) {
        emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
        emit_uint32(compiler, (uint32_t)local, (int)expr->line);
      } else {
        // Check for persistent global
        uint32_t global_idx = VM_GLOBAL_NOT_FOUND;
        if (compiler->registry) {
            global_idx = vm_registry_find_global(compiler->registry, name);
        }
        
        if (global_idx == VM_GLOBAL_NOT_FOUND && compiler->module) {
            const AstDecl* d = compiler->module->decls;
            while (d) {
                if (d->kind == AST_DECL_GLOBAL_LET && str_eq(d->as.let_decl.name, name)) {
                    if (compiler->registry) {
                        Str type_name_str = get_base_type_name(d->as.let_decl.type);
                        global_idx = vm_registry_ensure_global(compiler->registry, name, type_name_str);
                    }
                    break;
                }
                d = d->next;
            }
        }
        
        if (global_idx != VM_GLOBAL_NOT_FOUND) {
            emit_op(compiler, OP_GET_GLOBAL, (int)expr->line);
            emit_uint32(compiler, global_idx, (int)expr->line);
        } else {
            char buffer[128];
            snprintf(buffer, sizeof(buffer), "unknown identifier '%.*s' in VM", (int)name.len, name.data);
            diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
            compiler->had_error = true;
            return false;
        }
      }
      return true;
    }
    case AST_EXPR_STRING: {
      Value string_value = value_string_copy(expr->as.string_lit.data, expr->as.string_lit.len);
      emit_constant(compiler, string_value, (int)expr->line);
      return true;
    }
    case AST_EXPR_INTERP: {
      AstInterpPart* part = expr->as.interp.parts;
      if (!part) {
          emit_constant(compiler, value_string_copy("", 0), (int)expr->line);
          return true;
      }
      
      // Initial part (guaranteed to be a string based on parser)
      if (!compile_expr(compiler, part->value)) return false;
      part = part->next;
      
      while (part) {
          // Current stack has LHS. Decide if LHS needs wrapping.
          // Note: In the first iteration, LHS is the initial string part.
          // In subsequent iterations, it's the result of the previous rae_str_concat (String).
          
          // Push RHS
          if (!compile_expr(compiler, part->value)) return false;
          
          Str rhs_type = vm_infer_expr_type(compiler, part->value);
          
          // If the next value isn't a string (it's the {expression} result), wrap it in rae_str
          if (!str_eq_cstr(rhs_type, "String")) {
              emit_op(compiler, OP_NATIVE_CALL, (int)expr->line);
              const char* native_name = "rae_str";
              Str name_str = str_from_cstr(native_name);
              uint32_t name_idx = chunk_add_constant(compiler->chunk, value_string_copy(name_str.data, name_str.len));
              emit_uint32(compiler, name_idx, (int)expr->line);
              emit_byte(compiler, 1, (int)expr->line); // 1 arg
          }
          
          // Now stack has [LHS, RHS_str]. Call rae_str_concat
          emit_op(compiler, OP_NATIVE_CALL, (int)expr->line);
          const char* concat_name = "rae_str_concat";
          Str concat_str = str_from_cstr(concat_name);
          uint32_t concat_idx = chunk_add_constant(compiler->chunk, value_string_copy(concat_str.data, concat_str.len));
          emit_uint32(compiler, concat_idx, (int)expr->line);
          emit_byte(compiler, 2, (int)expr->line); // 2 args
          
          part = part->next;
      }
      return true;
    }
    case AST_EXPR_CHAR: {
      emit_constant(compiler, value_char(expr->as.char_value), (int)expr->line);
      return true;
    }
    case AST_EXPR_INTEGER: {
      long long parsed = strtoll(expr->as.integer.data, NULL, 10);
      emit_constant(compiler, value_int(parsed), (int)expr->line);
      return true;
    }
    case AST_EXPR_FLOAT: {
      double parsed = strtod(expr->as.floating.data, NULL);
      emit_constant(compiler, value_float(parsed), (int)expr->line);
      return true;
    }
    case AST_EXPR_BOOL: {
      emit_constant(compiler, value_bool(expr->as.boolean), (int)expr->line);
      return true;
    }
    case AST_EXPR_NONE: {
      emit_constant(compiler, value_none(), (int)expr->line);
      return true;
    }
    case AST_EXPR_CALL:
      return compile_call(compiler, expr, false);
    case AST_EXPR_MEMBER: {
      if (expr->as.member.object->kind == AST_EXPR_IDENT) {
          Str obj_name = expr->as.member.object->as.ident;
          // Check if it's an enum member access (e.g. Color.RED)
          EnumEntry* en = enum_table_find(&compiler->compiler_ctx->enums, obj_name);
          if (en) {
              int member_idx = enum_entry_find_member(en, expr->as.member.member);
              if (member_idx < 0) {
                  char buffer[128];
                  snprintf(buffer, sizeof(buffer), "enum '%.*s' has no member '%.*s'", 
                           (int)obj_name.len, obj_name.data,
                           (int)expr->as.member.member.len, expr->as.member.member.data);
                  diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
                  compiler->had_error = true;
                  return false;
              }
              emit_constant(compiler, value_int(member_idx), (int)expr->line);
              return true;
          }
      }

      Str obj_type_raw = vm_infer_expr_type(compiler, expr->as.member.object);
      Str type_name = get_base_type_name_str(obj_type_raw);
      if (type_name.len == 0) {
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                   "could not determine type of object for member access");
        compiler->had_error = true;
        return false;
      }
      TypeEntry* type = type_table_find(&compiler->compiler_ctx->types, type_name);
      if (!type) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "unknown type '%.*s'", (int)type_name.len, type_name.data);
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
        compiler->had_error = true;
        return false;
      }
      int field_index = type_entry_find_field(type, expr->as.member.member);
      if (field_index < 0) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "type '%.*s' has no field '%.*s'", 
                 (int)type_name.len, type_name.data,
                 (int)expr->as.member.member.len, expr->as.member.member.data);
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
        compiler->had_error = true;
        return false;
      }
      
      if (!compile_expr(compiler, expr->as.member.object)) return false;
      emit_op(compiler, OP_GET_FIELD, (int)expr->line);
      emit_uint32(compiler, (uint32_t)field_index, (int)expr->line);
      return true;
    }
    case AST_EXPR_BINARY: {
      switch (expr->as.binary.op) {
        case AST_BIN_AND: {
          if (!compile_expr(compiler, expr->as.binary.lhs)) {
            return false;
          }
          uint32_t end_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)expr->line);
          emit_op(compiler, OP_POP, (int)expr->line);
          if (!compile_expr(compiler, expr->as.binary.rhs)) {
            return false;
          }
          patch_jump(compiler, end_jump);
          return true;
        }
        case AST_BIN_OR: {
          if (!compile_expr(compiler, expr->as.binary.lhs)) {
            return false;
          }
          uint32_t false_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)expr->line);
          uint32_t end_jump = emit_jump(compiler, OP_JUMP, (int)expr->line);
          patch_jump(compiler, false_jump);
          emit_op(compiler, OP_POP, (int)expr->line);
          if (!compile_expr(compiler, expr->as.binary.rhs)) {
            return false;
          }
          patch_jump(compiler, end_jump);
          return true;
        }
        default:
          if (!compile_expr(compiler, expr->as.binary.lhs)) {
            return false;
          }
          if (!compile_expr(compiler, expr->as.binary.rhs)) {
            return false;
          }
          switch (expr->as.binary.op) {
            case AST_BIN_ADD:
              emit_op(compiler, OP_ADD, (int)expr->line);
              return true;
            case AST_BIN_SUB:
              emit_op(compiler, OP_SUB, (int)expr->line);
              return true;
            case AST_BIN_MUL:
              emit_op(compiler, OP_MUL, (int)expr->line);
              return true;
            case AST_BIN_DIV:
              emit_op(compiler, OP_DIV, (int)expr->line);
              return true;
            case AST_BIN_MOD:
              emit_op(compiler, OP_MOD, (int)expr->line);
              return true;
            case AST_BIN_LT:
              emit_op(compiler, OP_LT, (int)expr->line);
              return true;
            case AST_BIN_GT:
              emit_op(compiler, OP_GT, (int)expr->line);
              return true;
            case AST_BIN_LE:
              emit_op(compiler, OP_LE, (int)expr->line);
              return true;
            case AST_BIN_GE:
              emit_op(compiler, OP_GE, (int)expr->line);
              return true;
            case AST_BIN_IS:
              emit_op(compiler, OP_EQ, (int)expr->line);
              return true;
            default:
              diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                         "binary operator not supported in VM yet");
              compiler->had_error = true;
              return false;
          }
      }
    }
    case AST_EXPR_UNARY: {
      if (expr->as.unary.op == AST_UNARY_SPAWN) {
          const AstExpr* call = expr->as.unary.operand;
          if (call->kind != AST_EXPR_CALL) {
              diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "spawn must be followed by a function call");
              return false;
          }
          return compile_call(compiler, call, true);
      }

      if (!compile_expr(compiler, expr->as.unary.operand)) {
        return false;
      }
      if (expr->as.unary.op == AST_UNARY_NEG) {
        emit_op(compiler, OP_NEG, (int)expr->line);
        return true;
      } else if (expr->as.unary.op == AST_UNARY_NOT) {
        emit_op(compiler, OP_NOT, (int)expr->line);
        return true;
      } else if (expr->as.unary.op == AST_UNARY_VIEW || expr->as.unary.op == AST_UNARY_MOD) {
        bool is_mod = (expr->as.unary.op == AST_UNARY_MOD);
        const AstExpr* operand = expr->as.unary.operand;
        if (operand->kind == AST_EXPR_IDENT) {
          int slot = compiler_find_local(compiler, operand->as.ident);
          if (slot < 0) {
            diag_error(compiler->file_path, (int)operand->line, (int)operand->column, "unknown identifier");
            compiler->had_error = true; return false;
          }
          emit_op(compiler, is_mod ? OP_MOD_LOCAL : OP_VIEW_LOCAL, (int)expr->line);
          emit_uint32(compiler, (uint32_t)slot, (int)expr->line);
          return true;
        } else if (operand->kind == AST_EXPR_MEMBER) {
          if (!compile_expr(compiler, operand->as.member.object)) return false;
          Str type_name = vm_get_local_type_name(compiler, operand->as.member.object->as.ident);
          TypeEntry* type = type_table_find(&compiler->compiler_ctx->types, type_name);
          int field_index = type_entry_find_field(type, operand->as.member.member);
          
          emit_op(compiler, is_mod ? OP_MOD_FIELD : OP_VIEW_FIELD, (int)expr->line);
          emit_uint32(compiler, (uint32_t)field_index, (int)expr->line);
          return true;
        } else {
          diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "view/mod can only be applied to lvalues (identifiers or members)");
          compiler->had_error = true;
          return false;
        }
      } else if (expr->as.unary.op == AST_UNARY_SPAWN) {
          if (expr->as.unary.operand->kind != AST_EXPR_CALL) {
              diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "spawn must be followed by a function call");
              return false;
          }
          return compile_call(compiler, expr->as.unary.operand, true);
      } else if (expr->as.unary.op == AST_UNARY_PRE_INC || expr->as.unary.op == AST_UNARY_PRE_DEC ||
                 expr->as.unary.op == AST_UNARY_POST_INC || expr->as.unary.op == AST_UNARY_POST_DEC) {
        bool is_post = (expr->as.unary.op == AST_UNARY_POST_INC || expr->as.unary.op == AST_UNARY_POST_DEC);
        bool is_inc = (expr->as.unary.op == AST_UNARY_PRE_INC || expr->as.unary.op == AST_UNARY_POST_INC);
        const AstExpr* operand = expr->as.unary.operand;

        if (operand->kind == AST_EXPR_IDENT) {
            int slot = compiler_find_local(compiler, operand->as.ident);
            if (slot < 0) {
               diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                          "unknown identifier in increment/decrement");
               compiler->had_error = true;
               return false;
            }
            
            emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
            emit_uint32(compiler, (uint32_t)slot, (int)expr->line);
            
            if (is_post) {
                emit_op(compiler, OP_DUP, (int)expr->line);
            }
            
            emit_constant(compiler, value_int(1), (int)expr->line);
            emit_op(compiler, is_inc ? OP_ADD : OP_SUB, (int)expr->line);
            
            emit_op(compiler, OP_SET_LOCAL, (int)expr->line);
            emit_uint32(compiler, (uint32_t)slot, (int)expr->line);
            
            if (is_post) {
                emit_op(compiler, OP_POP, (int)expr->line); // remove new value, leave original
            }
            return true;
        } else if (operand->kind == AST_EXPR_MEMBER) {
            const AstExpr* obj_expr = operand->as.member.object;
            Str type_name = {0};
            if (obj_expr->kind == AST_EXPR_IDENT) {
                type_name = vm_get_local_type_name(compiler, obj_expr->as.ident);
            }
            if (type_name.len == 0) {
                diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                           "could not determine type for member increment/decrement");
                return false;
            }
            TypeEntry* type = type_table_find(&compiler->compiler_ctx->types, type_name);
            int field_index = type_entry_find_field(type, operand->as.member.member);
            if (field_index < 0) {
                diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "unknown field");
                return false;
            }

            if (obj_expr->kind == AST_EXPR_IDENT) {
                int obj_slot = compiler_find_local(compiler, obj_expr->as.ident);
                emit_op(compiler, OP_GET_LOCAL, (int)obj_expr->line);
                emit_uint32(compiler, (uint32_t)obj_slot, (int)obj_expr->line);
                
                emit_op(compiler, OP_GET_FIELD, (int)expr->line);
                emit_uint32(compiler, (uint32_t)field_index, (int)expr->line);
                
                if (is_post) emit_op(compiler, OP_DUP, (int)expr->line);
                
                emit_constant(compiler, value_int(1), (int)expr->line);
                emit_op(compiler, is_inc ? OP_ADD : OP_SUB, (int)expr->line);
                
                emit_op(compiler, OP_SET_LOCAL_FIELD, (int)expr->line);
                emit_uint32(compiler, (uint32_t)obj_slot, (int)expr->line);
                emit_uint32(compiler, (uint32_t)field_index, (int)expr->line);
                
                if (is_post) emit_op(compiler, OP_POP, (int)expr->line);
            } else {
                if (!compile_expr(compiler, obj_expr)) return false;
                emit_op(compiler, OP_DUP, (int)expr->line);
                
                emit_op(compiler, OP_GET_FIELD, (int)expr->line);
                emit_uint32(compiler, (uint32_t)field_index, (int)expr->line);
                
                if (is_post) emit_op(compiler, OP_DUP, (int)expr->line);
                
                emit_constant(compiler, value_int(1), (int)expr->line);
                emit_op(compiler, is_inc ? OP_ADD : OP_SUB, (int)expr->line);
                
                emit_op(compiler, OP_SET_FIELD, (int)expr->line);
                emit_uint32(compiler, (uint32_t)field_index, (int)expr->line);
                
                if (is_post) emit_op(compiler, OP_POP, (int)expr->line);
            }
            return true;
        }
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                   "increment/decrement operand must be an identifier or member in VM");
        compiler->had_error = true;
        return false;
      }
      diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                 "unary operator not supported in VM yet");
      compiler->had_error = true;
      return false;
    }
    case AST_EXPR_OBJECT: {
      Str type_name = {0};
      if (expr->as.object_literal.type && expr->as.object_literal.type->parts) {
          type_name = expr->as.object_literal.type->parts->text;
      } else {
          type_name = compiler->expected_type;
      }
      
      TypeEntry* type = type_name.len > 0 ? type_table_find(&compiler->compiler_ctx->types, type_name) : NULL;
      
      if (type) {
          // Reorder fields according to type definition
          Str saved_expected = compiler->expected_type;
          for (size_t i = 0; i < type->field_count; i++) {
              Str expected_name = type->field_names[i];
              compiler->expected_type = get_base_type_name(type->field_types[i]);
              
              const AstObjectField* f = expr->as.object_literal.fields;
              bool found = false;
              while (f) {
                  if (str_matches(f->name, expected_name)) {
                      if (!compile_expr(compiler, f->value)) return false;
                      found = true;
                      break;
                  }
                  f = f->next;
              }
              if (!found) {
                  // Push default value if field is missing in literal
                  if (type->field_defaults[i]) {
                      if (!compile_expr(compiler, type->field_defaults[i])) return false;
                  } else {
                      if (!emit_default_value(compiler, type->field_types[i], (int)expr->line)) {
                          return false;
                      }
                  }
              }
          }
          compiler->expected_type = saved_expected;
          emit_op(compiler, OP_CONSTRUCT, (int)expr->line);
          emit_uint32(compiler, (uint32_t)type->field_count, (int)expr->line);
          uint32_t type_idx = chunk_add_constant(compiler->chunk, value_string_copy(type->name.data, type->name.len));
          emit_uint32(compiler, type_idx, (int)expr->line);
      } else {
          // Untyped or unknown type: push in order of appearance
          uint32_t count = 0;
          const AstObjectField* f = expr->as.object_literal.fields;
          while (f) {
            if (!compile_expr(compiler, f->value)) return false;
            count++;
            f = f->next;
          }
          emit_op(compiler, OP_CONSTRUCT, (int)expr->line);
          emit_uint32(compiler, count, (int)expr->line);
          emit_uint32(compiler, 0xFFFFFFFF, (int)expr->line);
      }
      return true;
    }
    case AST_EXPR_MATCH: {
      int subject_slot = compiler_add_local(compiler, str_from_cstr("$match_subject"), (Str){0}, false, false);
      if (subject_slot < 0) {
        return false;
      }
      if (!compiler_ensure_local_capacity(compiler, compiler->local_count, (int)expr->line)) {
        return false;
      }
      if (!compile_expr(compiler, expr->as.match_expr.subject)) {
        return false;
      }
      emit_op(compiler, OP_SET_LOCAL, (int)expr->line);
      emit_uint32(compiler, (uint32_t)subject_slot, (int)expr->line);
      emit_op(compiler, OP_POP, (int)expr->line);

      int result_slot = compiler_add_local(compiler, str_from_cstr("$match_value"), (Str){0}, false, false);
      if (result_slot < 0) {
        return false;
      }
      if (!compiler_ensure_local_capacity(compiler, compiler->local_count, (int)expr->line)) {
        return false;
      }

      uint32_t end_jumps[256];
      size_t end_count = 0;
      bool has_default = false;
      AstMatchArm* arm = expr->as.match_expr.arms;
      while (arm) {
        bool is_default = arm->pattern == NULL;
        if (is_default) {
          has_default = true;
          if (!compile_expr(compiler, arm->value)) {
            return false;
          }
          emit_op(compiler, OP_SET_LOCAL, (int)expr->line);
          emit_uint32(compiler, (uint32_t)result_slot, (int)expr->line);
          emit_op(compiler, OP_POP, (int)expr->line);
          if (end_count < sizeof(end_jumps) / sizeof(end_jumps[0])) {
            end_jumps[end_count++] = emit_jump(compiler, OP_JUMP, (int)expr->line);
          }
        } else {
          emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
          emit_uint32(compiler, (uint32_t)subject_slot, (int)expr->line);
          if (!compile_expr(compiler, arm->pattern)) {
            return false;
          }
          emit_op(compiler, OP_EQ, (int)expr->line);
          uint32_t skip = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)expr->line);
          emit_op(compiler, OP_POP, (int)expr->line);
          if (!compile_expr(compiler, arm->value)) {
            return false;
          }
          emit_op(compiler, OP_SET_LOCAL, (int)expr->line);
          emit_uint32(compiler, (uint32_t)result_slot, (int)expr->line);
          emit_op(compiler, OP_POP, (int)expr->line);
          if (end_count < sizeof(end_jumps) / sizeof(end_jumps[0])) {
            end_jumps[end_count++] = emit_jump(compiler, OP_JUMP, (int)expr->line);
          }
          patch_jump(compiler, skip);
          emit_op(compiler, OP_POP, (int)expr->line);
        }
        arm = arm->next;
      }
      if (!has_default) {
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                   "match expression requires a 'default' arm");
        compiler->had_error = true;
        return false;
      }
      for (size_t i = 0; i < end_count; ++i) {
        patch_jump(compiler, end_jumps[i]);
      }
      emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
      emit_uint32(compiler, (uint32_t)result_slot, (int)expr->line);
      return true;
    }
    case AST_EXPR_METHOD_CALL: {
      Str method_name = expr->as.method_call.method_name;
      const AstExpr* receiver = expr->as.method_call.object;

      // Check if it's a module call like sys.exit()
      if (receiver->kind == AST_EXPR_IDENT && is_module_import(compiler, receiver->as.ident)) {
          Str mod_name = receiver->as.ident;
          uint32_t arg_count = 0;
          for (const AstCallArg* arg = expr->as.method_call.args; arg; arg = arg->next) arg_count++;
          
          Str* arg_types = NULL;
          if (arg_count > 0) {
              arg_types = malloc(arg_count * sizeof(Str));
              const AstCallArg* arg = expr->as.method_call.args;
              for (uint32_t i = 0; i < arg_count; i++) {
                  arg_types[i] = vm_infer_expr_type(compiler, arg->value);
                  arg = arg->next;
              }
          }
          
          // Match in function table (without mod prefix, as they are currently flat in FunctionTable)
          FunctionEntry* entry = function_table_find_overload(&compiler->compiler_ctx->functions, method_name, arg_types, arg_count);
          free(arg_types);
          
          if (entry) {
              // Compile args
              const AstCallArg* arg = expr->as.method_call.args;
              while (arg) {
                  if (!compile_expr(compiler, arg->value)) return false;
                  arg = arg->next;
              }
              return emit_function_call(compiler, entry, (int)expr->line, (int)expr->column, (uint8_t)arg_count);
          }
          
          // If not found in Rae functions, maybe it's a native with prefix mod.member
          char* combined = malloc(mod_name.len + method_name.len + 2);
          sprintf(combined, "%.*s.%.*s", (int)mod_name.len, mod_name.data, (int)method_name.len, method_name.data);
          Str full_name = str_from_buf(combined, strlen(combined));
          
          // Compile args
          const AstCallArg* arg = expr->as.method_call.args;
          while (arg) {
              if (!compile_expr(compiler, arg->value)) { free(combined); return false; }
              arg = arg->next;
          }
          
          bool ok = emit_native_call(compiler, full_name, (uint8_t)arg_count, (int)expr->line, (int)expr->column);
          free(combined);
          return ok;
      }

      // Handle built-in toString() for all types
      if (str_eq_cstr(method_name, "toString")) {
          if (!compile_expr(compiler, expr->as.method_call.object)) return false;
          return emit_native_call(compiler, str_from_cstr("rae_str"), 1, (int)expr->line, (int)expr->column);
      }

      // Handle built-in toJson() for all types
      if (str_eq_cstr(method_name, "toJson")) {
          if (!compile_expr(compiler, expr->as.method_call.object)) return false;
          return emit_native_call(compiler, str_from_cstr("rae_to_json"), 1, (int)expr->line, (int)expr->column);
      }

      // Handle built-in fromJson() for all types
      if (str_eq_cstr(method_name, "fromJson")) {
          if (expr->as.method_call.object->kind == AST_EXPR_IDENT) {
              Str type_name = expr->as.method_call.object->as.ident;
              TypeEntry* te = type_table_find(&compiler->compiler_ctx->types, type_name);
              if (te) {
                  // Push type name as first hidden arg
                  emit_constant(compiler, value_string_copy(type_name.data, type_name.len), (int)expr->line);
                  // Push JSON string arg
                  if (expr->as.method_call.args) {
                      if (!compile_expr(compiler, expr->as.method_call.args->value)) return false;
                  } else {
                      emit_constant(compiler, value_string_copy("", 0), (int)expr->line);
                  }
                  return emit_native_call(compiler, str_from_cstr("rae_from_json"), 2, (int)expr->line, (int)expr->column);
              }
          }
      }

      // Handle built-in toBinary() for all types
      if (str_eq_cstr(method_name, "toBinary")) {
          if (!compile_expr(compiler, expr->as.method_call.object)) return false;
          return emit_native_call(compiler, str_from_cstr("rae_to_binary"), 1, (int)expr->line, (int)expr->column);
      }

      // Handle built-in fromBinary() for all types
      if (str_eq_cstr(method_name, "fromBinary")) {
          if (expr->as.method_call.object->kind == AST_EXPR_IDENT) {
              Str type_name = expr->as.method_call.object->as.ident;
              TypeEntry* te = type_table_find(&compiler->compiler_ctx->types, type_name);
              if (te) {
                  emit_constant(compiler, value_string_copy(type_name.data, type_name.len), (int)expr->line);
                  if (expr->as.method_call.args) {
                      if (!compile_expr(compiler, expr->as.method_call.args->value)) return false;
                  } else {
                      diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "fromBinary expects 1 argument");
                      return false;
                  }
                  return emit_native_call(compiler, str_from_cstr("rae_from_binary"), 2, (int)expr->line, (int)expr->column);
              }
          }
      }

      FunctionEntry* entry = NULL;
      
      // Calculate total arguments (1 receiver + explicit args)
      uint32_t explicit_args_count = 0;
      for (const AstCallArg* arg = expr->as.method_call.args; arg; arg = arg->next) explicit_args_count++;
      uint32_t total_arg_count = 1 + explicit_args_count;

      // Infer all types for dispatch
      Str* arg_types = malloc(total_arg_count * sizeof(Str));
      arg_types[0] = get_base_type_name_str(vm_infer_expr_type(compiler, expr->as.method_call.object));
      {
          const AstCallArg* arg = expr->as.method_call.args;
          for (uint32_t i = 0; i < explicit_args_count; ++i) {
              arg_types[i + 1] = vm_infer_expr_type(compiler, arg->value);
              arg = arg->next;
          }
      }

      entry = function_table_find_overload(&compiler->compiler_ctx->functions, method_name, arg_types, total_arg_count);
      free(arg_types);

      // Compile the receiver ('this')
      
      // We need to decide whether to pass the receiver as a reference or a value.
      // For now, in the VM, we often prefer mod reference for potential mutation
      // unless it's a primitive.
      if (receiver->kind == AST_EXPR_IDENT) {
          int slot = compiler_find_local(compiler, receiver->as.ident);
          if (slot >= 0) {
              // Decide how to push receiver
              emit_op(compiler, OP_MOD_LOCAL, (int)expr->line);
              emit_uint32(compiler, (uint32_t)slot, (int)expr->line);
          } else {
              if (!compile_expr(compiler, receiver)) return false;
          }
      } else {
          if (!compile_expr(compiler, receiver)) return false;
          // If receiver is an rvalue, we might still need to wrap it if the func expects a ref
          // but for now let's just push it.
      }

      // Compile remaining arguments
      const AstCallArg* current_arg = expr->as.method_call.args;
      while (current_arg) {
        if (!compile_expr(compiler, current_arg->value)) return false;
        current_arg = current_arg->next;
      }

      if (!entry) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "unknown method '%.*s'", (int)method_name.len, method_name.data);
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
        compiler->had_error = true;
        return false;
      }

      return emit_function_call(compiler, entry, (int)expr->line, (int)expr->column, total_arg_count);
    }
    case AST_EXPR_INDEX: {
      Str target_type = vm_infer_expr_type(compiler, expr->as.index.target);
      if (!compile_expr(compiler, expr->as.index.target)) return false;
      if (!compile_expr(compiler, expr->as.index.index)) return false;
      // For index, we fallback to 'get' for the specific type
      Str param_types[] = { target_type, str_from_cstr("Int64") };
      FunctionEntry* entry = function_table_find_overload(&compiler->compiler_ctx->functions, str_from_cstr("get"), param_types, 2);
      if (!entry) {
          diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "built-in 'get' method not found for indexing this type");
          compiler->had_error = true;
          return false;
      }
      return emit_function_call(compiler, entry, (int)expr->line, (int)expr->column, 2);
    }
    case AST_EXPR_COLLECTION_LITERAL: {
      uint32_t element_count = 0;
      AstCollectionElement* current = expr->as.collection.elements;
      bool is_map = false;
      if (current && current->key) { // Check if the first element has a key to infer map
        is_map = true;
      }

      if (is_map) {
          while (current) {
            if (!compile_expr(compiler, current->value)) return false;
            element_count++;
            current = current->next;
          }
          emit_op(compiler, OP_CONSTRUCT, (int)expr->line);
          emit_uint32(compiler, element_count, (int)expr->line);
          emit_uint32(compiler, 0xFFFFFFFF, (int)expr->line);
          return true;
      } else {
          // Rae-native List construction
          while (current) { element_count++; current = current->next; }

          Str int_type = str_from_cstr("Int64");
    FunctionEntry* create_entry = function_table_find_overload(&compiler->compiler_ctx->functions, str_from_cstr("createList"), &int_type, 1);
          if (!create_entry) {
              diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "built-in 'createList' not found in core.rae");
              return false;
          }

          // Push initialCap
          emit_constant(compiler, value_int(element_count), (int)expr->line);
          if (!emit_function_call(compiler, create_entry, (int)expr->line, (int)expr->column, 1)) return false;

          // Store list in a temporary local to allow calling methods by reference
          char temp_name[64];
          snprintf(temp_name, sizeof(temp_name), "__list_lit_%zu_%zu", expr->line, expr->column);
          int slot = compiler_add_local(compiler, str_from_cstr(temp_name), str_from_cstr("List"), false, false);
          if (slot < 0) return false;
          if (!compiler_ensure_local_capacity(compiler, compiler->local_count, (int)expr->line)) return false;

          emit_op(compiler, OP_SET_LOCAL, (int)expr->line);
          emit_uint32(compiler, (uint32_t)slot, (int)expr->line);
          // Stack still has the list value (OP_SET_LOCAL leaves it there)
          // We pop it because we'll build it via the local ref and then push it back at the end
          emit_op(compiler, OP_POP, (int)expr->line);

          Str add_types[] = { str_from_cstr("List"), str_from_cstr("Any") };
          FunctionEntry* add_entry = function_table_find_overload(&compiler->compiler_ctx->functions, str_from_cstr("add"), add_types, 2);
          if (!add_entry) {
              diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "built-in 'add' not found in core.rae");
              return false;
          }

          current = expr->as.collection.elements;
          while (current) {
              // Push mod ref to list
              emit_op(compiler, OP_MOD_LOCAL, (int)expr->line);
              emit_uint32(compiler, (uint32_t)slot, (int)expr->line);

              // Compile element value
              if (!compile_expr(compiler, current->value)) return false;

              // Call listAdd(list, value)
              if (!emit_function_call(compiler, add_entry, (int)expr->line, (int)expr->column, 2)) return false;

              // Pop 'none' result of add
              emit_op(compiler, OP_POP, (int)expr->line);

              current = current->next;
          }

          // Push the final list back onto the stack
          emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
          emit_uint32(compiler, (uint32_t)slot, (int)expr->line);
          return true;
      }
    }
    case AST_EXPR_LIST: {
      uint32_t element_count = 0;
      AstExprList* current = expr->as.list;
      while (current) { element_count++; current = current->next; }

      Str int_type = str_from_cstr("Int64");
    FunctionEntry* create_entry = function_table_find_overload(&compiler->compiler_ctx->functions, str_from_cstr("createList"), &int_type, 1);
      if (!create_entry) {
          diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "built-in 'createList' not found in core.rae");
          return false;
      }

      emit_constant(compiler, value_int(element_count), (int)expr->line);
      if (!emit_function_call(compiler, create_entry, (int)expr->line, (int)expr->column, 1)) return false;

      char temp_name[64];
      snprintf(temp_name, sizeof(temp_name), "__list_lit_%zu_%zu", expr->line, expr->column);
      int slot = compiler_add_local(compiler, str_from_cstr(temp_name), str_from_cstr("List"), false, false);
      if (slot < 0) return false;
      if (!compiler_ensure_local_capacity(compiler, compiler->local_count, (int)expr->line)) return false;

      emit_op(compiler, OP_SET_LOCAL, (int)expr->line);
      emit_uint32(compiler, (uint32_t)slot, (int)expr->line);
      emit_op(compiler, OP_POP, (int)expr->line);

      Str add_types[] = { str_from_cstr("List"), str_from_cstr("Any") };
      FunctionEntry* add_entry = function_table_find_overload(&compiler->compiler_ctx->functions, str_from_cstr("add"), add_types, 2);
      if (!add_entry) {
          diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "built-in 'add' not found in core.rae");
          return false;
      }

      current = expr->as.list;
      while (current) {
          emit_op(compiler, OP_MOD_LOCAL, (int)expr->line);
          emit_uint32(compiler, (uint32_t)slot, (int)expr->line);
          if (!compile_expr(compiler, current->value)) return false;
          if (!emit_function_call(compiler, add_entry, (int)expr->line, (int)expr->column, 2)) return false;
          emit_op(compiler, OP_POP, (int)expr->line);
          current = current->next;
      }

      emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
      emit_uint32(compiler, (uint32_t)slot, (int)expr->line);
      return true;
    }
    case AST_EXPR_BOX:
    case AST_EXPR_UNBOX:
      // In current VM, values carry their own type tags, no-op conversion
      return compile_expr(compiler, expr->as.unary.operand);
    default:
      diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                 "expression not supported in VM yet");
      compiler->had_error = true;
      return false;
  }
  return false;
}
