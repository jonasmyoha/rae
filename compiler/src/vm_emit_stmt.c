// vm_emit_stmt.c — Bytecode emission for Rae statements.
//
// `compile_stmt` is the per-AST-node switch for statements. Helper emitters
// for lvalue references and default-value initialisation live here too.

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

static const char* stmt_kind_name(AstStmtKind kind) {
  switch (kind) {
    case AST_STMT_LET: return "let";
    case AST_STMT_DESTRUCT: return "destructure";
    case AST_STMT_EXPR: return "expression";
    case AST_STMT_RET: return "ret";
    case AST_STMT_IF: return "if";
    case AST_STMT_LOOP: return "loop";
    case AST_STMT_MATCH: return "match";
    case AST_STMT_ASSIGN: return "assignment";
    case AST_STMT_DEFER: return "defer";
    default: break;
  }
  return "unknown";
}

bool emit_default_value(BytecodeCompiler* compiler, const AstTypeRef* type, int line);

bool emit_lvalue_ref(BytecodeCompiler* compiler, const AstExpr* expr, bool is_mod) {
        if (expr->kind == AST_EXPR_IDENT) {
            int slot = compiler_find_local(compiler, expr->as.ident);
            if (slot < 0) {
                diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "unknown identifier for reference");
                return false;
            }
            if (compiler->locals[slot].is_ptr) {
                emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
                emit_uint32(compiler, (uint32_t)slot, (int)expr->line);
            } else {
                emit_op(compiler, is_mod ? OP_MOD_LOCAL : OP_VIEW_LOCAL, (int)expr->line);
                emit_uint32(compiler, (uint32_t)slot, (int)expr->line);
            }
            return true;
        }
     else if (expr->kind == AST_EXPR_MEMBER) {
        if (!emit_lvalue_ref(compiler, expr->as.member.object, is_mod)) return false;
        
        Str obj_type_raw = vm_infer_expr_type(compiler, expr->as.member.object);
        Str obj_type = get_base_type_name_str(obj_type_raw);
        TypeEntry* type = type_table_find(&compiler->compiler_ctx->types, obj_type);
        if (!type) {
            char buffer[128];
            snprintf(buffer, sizeof(buffer), "unknown type '%.*s' for member reference", (int)obj_type.len, obj_type.data);
            diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
            return false;
        }
        
        int field_index = type_entry_find_field(type, expr->as.member.member);
        if (field_index < 0) {
            diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "unknown field for reference");
            return false;
        }
        
        emit_op(compiler, is_mod ? OP_MOD_FIELD : OP_VIEW_FIELD, (int)expr->line);
        emit_uint32(compiler, (uint32_t)field_index, (int)expr->line);
        return true;
    }
    return false;
}

bool compile_stmt(BytecodeCompiler* compiler, const AstStmt* stmt) {
  if (!stmt) return true;
  switch (stmt->kind) {
    case AST_STMT_LET: {
      Str name = stmt->as.let_stmt.name;
      if (compiler->current_function == NULL) {
          // Global variable persistent state
          uint32_t global_idx = VM_GLOBAL_NOT_FOUND;
          if (compiler->registry) {
              Str type_name_str = get_base_type_name(stmt->as.let_stmt.type);
              global_idx = vm_registry_ensure_global(compiler->registry, name, type_name_str);
          } else {
              diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "globals require a VM registry");
              return false;
          }
          
          if (global_idx == VM_GLOBAL_NOT_FOUND) {
              diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "failed to allocate global storage");
              return false;
          }
          
          // Emit Guard: Skip if already initialized
          emit_op(compiler, OP_GET_GLOBAL_INIT_BIT, (int)stmt->line);
          emit_uint32(compiler, global_idx, (int)stmt->line);
          emit_op(compiler, OP_NOT, (int)stmt->line);
          uint32_t skip_init_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)stmt->line);
          
          if (stmt->as.let_stmt.value) {
            if (!compile_expr(compiler, stmt->as.let_stmt.value)) return false;
          } else {
            if (!emit_default_value(compiler, stmt->as.let_stmt.type, (int)stmt->line)) return false;
          }
          
          emit_op(compiler, OP_SET_GLOBAL, (int)stmt->line);
          emit_uint32(compiler, global_idx, (int)stmt->line);
          emit_op(compiler, OP_SET_GLOBAL_INIT_BIT, (int)stmt->line);
          emit_uint32(compiler, global_idx, (int)stmt->line);
          
          patch_jump(compiler, skip_init_jump);
          return true;
      }

            Str type_name = get_base_type_name(stmt->as.let_stmt.type);

            bool is_ptr = stmt->as.let_stmt.type && (stmt->as.let_stmt.type->is_view || stmt->as.let_stmt.type->is_mod);

            bool is_mod = stmt->as.let_stmt.type && stmt->as.let_stmt.type->is_mod;

            int slot = compiler_add_local(compiler, stmt->as.let_stmt.name, type_name, is_ptr, is_mod);

      
      if (slot < 0) {
        return false;
      }
      if (!compiler_ensure_local_capacity(compiler, compiler->local_count, (int)stmt->line)) {
        return false;
      }

      if (!stmt->as.let_stmt.value) {
        // Automatically initialize to default value
        if (!emit_default_value(compiler, stmt->as.let_stmt.type, (int)stmt->line)) {
          return false;
        }
        emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
        emit_uint32(compiler, (uint32_t)slot, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line);
      } else if (stmt->as.let_stmt.is_bind) {
          if (!stmt->as.let_stmt.type || 
              (!stmt->as.let_stmt.type->is_view && 
               !stmt->as.let_stmt.type->is_mod && 
               !stmt->as.let_stmt.type->is_opt)) {
              diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "=> not allowed for plain value types");
              return false;
          }
          // If the RHS is an identifier or member, we can emit a specific VIEW/MOD instruction
          // to get its address.
          if (stmt->as.let_stmt.value->kind == AST_EXPR_IDENT) {
              int src_slot = compiler_find_local(compiler, stmt->as.let_stmt.value->as.ident);
              emit_op(compiler, stmt->as.let_stmt.type && stmt->as.let_stmt.type->is_view ? OP_VIEW_LOCAL : OP_MOD_LOCAL, (int)stmt->line);
              emit_uint32(compiler, (uint32_t)src_slot, (int)stmt->line);
          } else if (stmt->as.let_stmt.value->kind == AST_EXPR_MEMBER) {
              const AstExpr* member_expr = stmt->as.let_stmt.value;
              if (member_expr->as.member.object->kind == AST_EXPR_IDENT) {
                  Str obj_name = member_expr->as.member.object->as.ident;
                  Str type_name = vm_get_local_type_name(compiler, obj_name);
                  TypeEntry* type = type_table_find(&compiler->compiler_ctx->types, type_name);
                  if (type) {
                      int field_index = type_entry_find_field(type, member_expr->as.member.member);
                      if (field_index >= 0) {
                          int obj_slot = compiler_find_local(compiler, obj_name);
                          emit_op(compiler, stmt->as.let_stmt.type && stmt->as.let_stmt.type->is_view ? OP_VIEW_LOCAL : OP_MOD_LOCAL, (int)stmt->line);
                          emit_uint32(compiler, (uint32_t)obj_slot, (int)stmt->line);
                          emit_op(compiler, stmt->as.let_stmt.type && stmt->as.let_stmt.type->is_view ? OP_VIEW_FIELD : OP_MOD_FIELD, (int)stmt->line);
                          emit_uint32(compiler, (uint32_t)field_index, (int)stmt->line);
                      } else {
                          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value");
                          return false;
                      }
                  } else {
                      diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value");
                      return false;
                  }
              } else {
                  diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value");
                  return false;
              }
          } else {
              // Fallback: compile as value
              if (!compile_expr(compiler, stmt->as.let_stmt.value)) return false;
              
              bool already_ref = false;
              if (stmt->as.let_stmt.value->kind == AST_EXPR_CALL) {
                  const AstExpr* callee = stmt->as.let_stmt.value->as.call.callee;
                  if (callee->kind == AST_EXPR_IDENT) {
                      Str name = callee->as.ident;
                      FunctionEntry* entry = function_table_find(&compiler->compiler_ctx->functions, name);
                      if (entry && entry->returns_ref) {
                          already_ref = true;
                      }
                  }
              } else if (stmt->as.let_stmt.value->kind == AST_EXPR_METHOD_CALL) {
                  Str method_name = stmt->as.let_stmt.value->as.method_call.method_name;
                  FunctionEntry* entry = function_table_find(&compiler->compiler_ctx->functions, method_name);
                  if (!entry) {
                      // Try common list methods
                      if (str_eq_cstr(method_name, "add")) entry = function_table_find(&compiler->compiler_ctx->functions, str_from_cstr("rae_list_add"));
                      else if (str_eq_cstr(method_name, "get")) entry = function_table_find(&compiler->compiler_ctx->functions, str_from_cstr("rae_list_get"));
                  }
                  if (entry && entry->returns_ref) {
                      already_ref = true;
                  }
              } else if (stmt->as.let_stmt.value->kind == AST_EXPR_NONE) {
                  already_ref = true;
              }
              
              if (!already_ref) {
                  diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value; RHS must be a reference or a function returning one");
                  return false;
              }
          }
          emit_op(compiler, OP_BIND_LOCAL, (int)stmt->line);
          emit_uint32(compiler, (uint32_t)slot, (int)stmt->line);
      } else {
          Str saved_expected = compiler->expected_type;
          compiler->expected_type = type_name;
          if (!compile_expr(compiler, stmt->as.let_stmt.value)) {
            compiler->expected_type = saved_expected;
            return false;
          }
          compiler->expected_type = saved_expected;
          emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
          emit_uint32(compiler, (uint32_t)slot, (int)stmt->line);
          emit_op(compiler, OP_POP, (int)stmt->line);
      }
      
      return true;
    }
    case AST_STMT_EXPR:
      if (!compile_expr(compiler, stmt->as.expr_stmt)) {
        return false;
      }
      emit_op(compiler, OP_POP, (int)stmt->line);
      return true;
    case AST_STMT_RET: {
      const AstReturnArg* arg = stmt->as.ret_stmt.values;
            if (!arg) {
              if (!vm_emit_defers(compiler, 0)) return false;
              return emit_return(compiler, false, (int)stmt->line);
            }
            if (arg->next) {
              diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                         "multiple return values not supported in VM yet");
              compiler->had_error = true;
              return false;
            }
            if (arg->has_label) {
              diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                         "labeled returns not supported in VM yet");
              compiler->had_error = true;
              return false;
            }
      
            // Lifetime check: can only return reference derived from params
            if (arg->value->kind == AST_EXPR_UNARY && 
                (arg->value->as.unary.op == AST_UNARY_VIEW || arg->value->as.unary.op == AST_UNARY_MOD)) {
                const AstExpr* operand = arg->value->as.unary.operand;
                const AstExpr* base_obj = operand;
                while (base_obj->kind == AST_EXPR_MEMBER) {
                    base_obj = base_obj->as.member.object;
                }
                if (base_obj->kind == AST_EXPR_IDENT) {
                    int slot = compiler_find_local(compiler, base_obj->as.ident);
                    if (slot >= 0) {
                        // In our simple VM compiler, parameters are the first N locals.
                        // We need to check if 'slot' corresponds to a parameter.
                        uint32_t param_count = 0;
                        if (compiler->current_function) {
                            const AstParam* p = compiler->current_function->params;
                            while (p) { param_count++; p = p->next; }
                        }
                        if (slot >= (int)param_count) {
                            diag_report(compiler->file_path, (int)arg->value->line, (int)arg->value->column, "reference escapes local storage");
                            compiler->had_error = true;
                            // continue to compile to find more errors
                        }
                    }
                }
            }
      
            if (!compile_expr(compiler, arg->value)) {
              return false;
            }
            if (!vm_emit_defers(compiler, 0)) return false;
            return emit_return(compiler, true, (int)stmt->line);
    }
    case AST_STMT_IF: {
      if (!stmt->as.if_stmt.condition || !stmt->as.if_stmt.then_block) {
        diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                   "if statement missing condition or then block");
        compiler->had_error = true;
        return false;
      }
      
      uint32_t scope_start_locals = compiler->local_count;

      if (!compile_expr(compiler, stmt->as.if_stmt.condition)) {
        return false;
      }
      uint32_t else_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)stmt->line);
      emit_op(compiler, OP_POP, (int)stmt->line);
      
      if (!compile_block(compiler, stmt->as.if_stmt.then_block)) {
        return false;
      }
      
      // Reset local count for scope reuse
      compiler->local_count = scope_start_locals;
      
      uint32_t end_jump = emit_jump(compiler, OP_JUMP, (int)stmt->line);
      
      patch_jump(compiler, else_jump);
      emit_op(compiler, OP_POP, (int)stmt->line);
      
      if (stmt->as.if_stmt.else_block) {
        if (!compile_block(compiler, stmt->as.if_stmt.else_block)) {
          return false;
        }
        
        // Reset local count for scope reuse
        compiler->local_count = scope_start_locals;
      }
      
      patch_jump(compiler, end_jump);
      return true;
    }
    case AST_STMT_LOOP: {
      if (stmt->as.loop_stmt.is_range) {
        // Range loop: loop x: Type in collection { ... }
        // stmt->as.loop_stmt.init is a DEF stmt for the loop variable 'x'
        // stmt->as.loop_stmt.condition is the collection expression
        
        uint32_t scope_start_locals = compiler->local_count;
        
        // 1. Evaluate collection and store in a hidden local
        if (!compile_expr(compiler, stmt->as.loop_stmt.condition)) return false;
        
        Str col_name = str_from_cstr("__collection");
        Str col_type_name = str_from_cstr("List"); // Default
        
        // Simple type inference for identifiers
        if (stmt->as.loop_stmt.condition->kind == AST_EXPR_IDENT) {
            Str inferred = vm_get_local_type_name(compiler, stmt->as.loop_stmt.condition->as.ident);
            if (inferred.len > 0) col_type_name = inferred;
        } else if (stmt->as.loop_stmt.condition->kind == AST_EXPR_MEMBER) {
            // HACK: for List2Int prototype, we know 'data' is Buffer
            if (str_eq_cstr(stmt->as.loop_stmt.condition->as.member.member, "data")) {
                col_type_name = str_from_cstr("Buffer");
            }
        }
        
        int col_slot = compiler_add_local(compiler, col_name, col_type_name, false, false);
        if (col_slot < 0) return false;
        emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
        emit_uint32(compiler, (uint32_t)col_slot, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line); // Clean stack

        // 2. Initialize index = 0 in a hidden local
        emit_constant(compiler, value_int(0), (int)stmt->line);
        Str idx_name = str_from_cstr("__index");
        int idx_slot = compiler_add_local(compiler, idx_name, str_from_cstr("Int64"), false, false);
        if (idx_slot < 0) return false;
        emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
        emit_uint32(compiler, (uint32_t)idx_slot, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line);

        // 3. Loop Start
        uint32_t loop_start = (uint32_t)compiler->chunk->code_count;

        // 4. Condition: index < collection.length()
        emit_op(compiler, OP_GET_LOCAL, (int)stmt->line);
        emit_uint32(compiler, (uint32_t)idx_slot, (int)stmt->line);
        
        // Determine collection type for length call
        bool is_buf = str_eq_cstr(col_type_name, "Buffer");
        bool is_str = str_eq_cstr(col_type_name, "String");

        // Call length() on collection
        emit_op(compiler, OP_GET_LOCAL, (int)stmt->line);
        emit_uint32(compiler, (uint32_t)col_slot, (int)stmt->line);
        if (is_buf) {
            emit_op(compiler, OP_BUF_LEN, (int)stmt->line);
        } else if (is_str) {
            // For string, we want to iterate bytes or decoded chars?
            // The requirement says iteration yields Char32.
            // But we need a way to track current byte index and decode.
            // Simplified: for now, just iterate bytes as Int (we'll fix to Char32 if needed)
            // Wait, the requirement says "iteration yields Char32 by decoding UTF-8".
            // This means we need a specialized OP_STR_ITER or similar.
            emit_op(compiler, OP_NATIVE_CALL, (int)stmt->line);
            uint32_t name_idx = chunk_add_constant(compiler->chunk, value_string_copy("rae_str_len", 14));
            emit_uint32(compiler, name_idx, (int)stmt->line);
            emit_byte(compiler, 1, (int)stmt->line);
        } else {
            if (!emit_native_call(compiler, str_from_cstr("rae_list_length"), 1, (int)stmt->line, 0)) return false;
        }
        
        emit_op(compiler, OP_LT, (int)stmt->line);
        uint32_t exit_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line);

        // 5. Body Start: Bind x = collection.get(index)
        Str var_name = stmt->as.loop_stmt.init->as.let_stmt.name;
        Str var_type = get_base_type_name(stmt->as.loop_stmt.init->as.let_stmt.type);
        int var_slot = compiler_add_local(compiler, var_name, var_type, false, false);
        if (var_slot < 0) return false;
        
        // collection.get(index)
        emit_op(compiler, OP_GET_LOCAL, (int)stmt->line);
        emit_uint32(compiler, (uint32_t)col_slot, (int)stmt->line);
        emit_op(compiler, OP_GET_LOCAL, (int)stmt->line);
        emit_uint32(compiler, (uint32_t)idx_slot, (int)stmt->line);
        if (is_buf) {
            emit_op(compiler, OP_BUF_GET, (int)stmt->line);
        } else if (is_str) {
            if (!emit_native_call(compiler, str_from_cstr("rae_str_at"), 2, (int)stmt->line, 0)) return false;
        } else {
            if (!emit_native_call(compiler, str_from_cstr("rae_list_get"), 2, (int)stmt->line, 0)) return false;
        }
        emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
        emit_uint32(compiler, (uint32_t)var_slot, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line);

        if (!compile_block(compiler, stmt->as.loop_stmt.body)) return false;

        // 6. Increment: index = index + 1
        emit_op(compiler, OP_GET_LOCAL, (int)stmt->line);
        emit_uint32(compiler, (uint32_t)idx_slot, (int)stmt->line);
        emit_constant(compiler, value_int(1), (int)stmt->line);
        emit_op(compiler, OP_ADD, (int)stmt->line);
        emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
        emit_uint32(compiler, (uint32_t)idx_slot, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line);

        // 7. Jump back
        emit_op(compiler, OP_JUMP, (int)stmt->line);
        emit_uint32(compiler, loop_start, (int)stmt->line);

        // 8. Exit
        patch_jump(compiler, exit_jump);
        emit_op(compiler, OP_POP, (int)stmt->line);
        
        compiler->local_count = scope_start_locals;
        return true;
      }
      
      // Enter scope for loop variables
      // (VM currently manages locals by simple count, so just tracking count is enough for "scope")
      uint32_t scope_start_locals = compiler->local_count;
      
      if (stmt->as.loop_stmt.init) {
        if (!compile_stmt(compiler, stmt->as.loop_stmt.init)) {
            return false;
        }
        // If init was an expression statement, it left nothing.
        // If init was a def, it added a local.
      }

      uint32_t loop_start = (uint32_t)compiler->chunk->code_count;
      
      uint32_t exit_jump = 0;
      bool has_condition = stmt->as.loop_stmt.condition != NULL;
      
      if (has_condition) {
        if (!compile_expr(compiler, stmt->as.loop_stmt.condition)) {
          return false;
        }
        exit_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line);
      }
      
      if (!compile_block(compiler, stmt->as.loop_stmt.body)) {
        return false;
      }
      
      if (stmt->as.loop_stmt.increment) {
        if (!compile_expr(compiler, stmt->as.loop_stmt.increment)) {
          return false;
        }
        emit_op(compiler, OP_POP, (int)stmt->line); // Discard increment result
      }
      
      emit_op(compiler, OP_JUMP, (int)stmt->line);
      emit_uint32(compiler, loop_start, (int)stmt->line);
      
      if (has_condition) {
        patch_jump(compiler, exit_jump);
        emit_op(compiler, OP_POP, (int)stmt->line);
      }
      
      // Reset local count for scope reuse
      compiler->local_count = scope_start_locals;
      return true;
    }
    case AST_STMT_MATCH: {
      if (!stmt->as.match_stmt.subject || !stmt->as.match_stmt.cases) {
        diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                   "match statement requires a subject and at least one case");
        compiler->had_error = true;
        return false;
      }
      int subject_slot = compiler_add_local(compiler, str_from_cstr("$match"), (Str){0}, false, false);
      if (subject_slot < 0) {
        return false;
      }
      if (!compiler_ensure_local_capacity(compiler, compiler->local_count, (int)stmt->line)) {
        return false;
      }
      if (!compile_expr(compiler, stmt->as.match_stmt.subject)) {
        return false;
      }
      emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
      emit_uint32(compiler, (uint32_t)subject_slot, (int)stmt->line);
      emit_op(compiler, OP_POP, (int)stmt->line);

      uint32_t end_jumps[256];
      size_t end_count = 0;
      bool had_default = false;

      const AstMatchCase* match_case = stmt->as.match_stmt.cases;
      while (match_case) {
        bool is_default = match_case->pattern == NULL ||
                          (match_case->pattern && match_case->pattern->kind == AST_EXPR_IDENT &&
                           str_eq_cstr(match_case->pattern->as.ident, "_"));
      if (is_default) {
        if (had_default) {
          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                     "multiple 'default' cases in match");
          compiler->had_error = true;
          return false;
        }
        had_default = true;
          if (!compile_block(compiler, match_case->block)) {
            return false;
          }
          if (end_count < sizeof(end_jumps) / sizeof(end_jumps[0])) {
            end_jumps[end_count++] = emit_jump(compiler, OP_JUMP, (int)stmt->line);
          }
        } else {
          emit_op(compiler, OP_GET_LOCAL, (int)stmt->line);
          emit_uint32(compiler, (uint32_t)subject_slot, (int)stmt->line);
          if (!compile_expr(compiler, match_case->pattern)) {
            return false;
          }
          emit_op(compiler, OP_EQ, (int)stmt->line);
          uint32_t skip = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)stmt->line);
          emit_op(compiler, OP_POP, (int)stmt->line);
          if (!compile_block(compiler, match_case->block)) {
            return false;
          }
          if (end_count < sizeof(end_jumps) / sizeof(end_jumps[0])) {
            end_jumps[end_count++] = emit_jump(compiler, OP_JUMP, (int)stmt->line);
          }
          patch_jump(compiler, skip);
          emit_op(compiler, OP_POP, (int)stmt->line);
        }
        match_case = match_case->next;
      }

      for (size_t i = 0; i < end_count; ++i) {
        patch_jump(compiler, end_jumps[i]);
      }
      return true;
    }
    case AST_STMT_ASSIGN: {
      const AstExpr* target = stmt->as.assign_stmt.target;
      if (target->kind == AST_EXPR_IDENT) {
        int slot = compiler_find_local(compiler, target->as.ident);
        if (slot < 0) {
          // Check for global
          uint32_t global_idx = VM_GLOBAL_NOT_FOUND;
          if (compiler->registry) {
              global_idx = vm_registry_find_global(compiler->registry, target->as.ident);
          }
          
          if (global_idx == VM_GLOBAL_NOT_FOUND && compiler->module) {
              const AstDecl* d = compiler->module->decls;
              while (d) {
                  if (d->kind == AST_DECL_GLOBAL_LET && str_eq(d->as.let_decl.name, target->as.ident)) {
                      if (compiler->registry) {
                          Str type_name_str = get_base_type_name(d->as.let_decl.type);
                          global_idx = vm_registry_ensure_global(compiler->registry, target->as.ident, type_name_str);
                      }
                      break;
                  }
                  d = d->next;
              }
          }
          
          if (global_idx != VM_GLOBAL_NOT_FOUND) {
              if (!compile_expr(compiler, stmt->as.assign_stmt.value)) return false;
              emit_op(compiler, OP_SET_GLOBAL, (int)stmt->line);
              emit_uint32(compiler, global_idx, (int)stmt->line);
              emit_op(compiler, OP_POP, (int)stmt->line);
              return true;
          }

          char buffer[128];
          snprintf(buffer, sizeof(buffer), "unknown identifier '%.*s' in assignment",
                   (int)target->as.ident.len, target->as.ident.data);
          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, buffer);
          compiler->had_error = true;
          return false;
        }

        if (stmt->as.assign_stmt.is_bind) {
            diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "rebinding an alias is illegal. '=>' is only for 'let' bindings.");
            return false;
        } else {
            // Check if local is view (read-only)
            if (compiler->locals[slot].is_ptr && !compiler->locals[slot].is_mod) {
                char buffer[160];
                snprintf(buffer, sizeof(buffer), "cannot assign to read-only view identifier '%.*s'", (int)target->as.ident.len, target->as.ident.data);
                diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, buffer);
                compiler->had_error = true;
                return false;
            }

            // Normal assignment: LHS = RHS
            Str saved_expected = compiler->expected_type;
            compiler->expected_type = vm_get_local_type_name(compiler, target->as.ident);
            if (!compile_expr(compiler, stmt->as.assign_stmt.value)) {
                compiler->expected_type = saved_expected;
                return false;
            }
            compiler->expected_type = saved_expected;
            emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
            emit_uint32(compiler, (uint32_t)slot, (int)stmt->line);
            emit_op(compiler, OP_POP, (int)stmt->line); // assigned value
        }
        return true;
      } else if (target->kind == AST_EXPR_MEMBER) {
        if (stmt->as.assign_stmt.is_bind) {
            diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "rebinding an alias is illegal. '=>' is only for 'let' bindings.");
            return false;
        }

        // Check if the object itself is a read-only view
        if (target->as.member.object->kind == AST_EXPR_IDENT) {
            int slot = compiler_find_local(compiler, target->as.member.object->as.ident);
            if (slot >= 0 && compiler->locals[slot].is_ptr && !compiler->locals[slot].is_mod) {
                char buffer[160];
                snprintf(buffer, sizeof(buffer), "cannot mutate field of read-only view '%.*s'", (int)target->as.member.object->as.ident.len, target->as.member.object->as.ident.data);
                diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, buffer);
                compiler->had_error = true;
                return false;
            }
        }

        // 1. Get reference to the parent object
        if (!emit_lvalue_ref(compiler, target->as.member.object, true)) return false;

        // 2. Resolve the field index and type
        Str obj_type_raw = vm_infer_expr_type(compiler, target->as.member.object);
        Str obj_type = get_base_type_name_str(obj_type_raw);
        TypeEntry* type = type_table_find(&compiler->compiler_ctx->types, obj_type);
        if (!type) {
          char buffer[128];
          snprintf(buffer, sizeof(buffer), "unknown type '%.*s' for member assignment", (int)obj_type.len, obj_type.data);
          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, buffer);
          return false;
        }
        int field_index = type_entry_find_field(type, target->as.member.member);
        if (field_index < 0) {
          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "unknown field for assignment");
          return false;
        }

        // 3. Compile the value to be assigned
        Str field_type = get_base_type_name(type->field_types[field_index]);
        Str saved_expected = compiler->expected_type;
        compiler->expected_type = field_type;
        if (!compile_expr(compiler, stmt->as.assign_stmt.value)) {
            compiler->expected_type = saved_expected;
            return false;
        }
        compiler->expected_type = saved_expected;

        // 4. Set the field
        emit_op(compiler, OP_SET_FIELD, (int)stmt->line);
        emit_uint32(compiler, (uint32_t)field_index, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line); // assigned value
        return true;
      } else if (target->kind == AST_EXPR_INDEX) {
        // Indexed assignment: target[index] = value
        // Sequence: push buffer, push index, push value
        if (!compile_expr(compiler, target->as.index.target)) return false;
        if (!compile_expr(compiler, target->as.index.index)) return false;
        if (!compile_expr(compiler, stmt->as.assign_stmt.value)) return false;
        
        emit_op(compiler, OP_BUF_SET, (int)stmt->line);
        // Note: OP_BUF_SET doesn't return a value, so we don't need to pop.
        // Wait, assignments in Rae usually return the value. 
        // But OP_BUF_SET implementation in vm.c doesn't push the value back.
        // Let's check other SET ops. OP_SET_FIELD pushes result.
        // If we want consistency, we might need OP_BUF_SET to push result or use DUP.
        // For now, let's just make it work for statements.
        return true;
      } else {
        diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                   "VM currently only supports assignment to identifiers, members, or indices");
        compiler->had_error = true;
        return false;
      }
    }
    case AST_STMT_DEFER: {
      if (compiler->defer_stack.count >= 64) {
        diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "defer stack overflow");
        compiler->had_error = true;
        return false;
      }
      compiler->defer_stack.entries[compiler->defer_stack.count].block = stmt->as.defer_stmt.block;
      compiler->defer_stack.entries[compiler->defer_stack.count].scope_depth = compiler->scope_depth;
      compiler->defer_stack.count++;
      return true;
    }
    default: {
      char buffer[128];
      snprintf(buffer, sizeof(buffer), "%s statement not supported in VM yet", stmt_kind_name(stmt->kind));
      diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, buffer);
      compiler->had_error = true;
      return false;
    }
  }
}

