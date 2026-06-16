// vm_emit_stmt.c — Bytecode emission for Rae statements.
//
// `compile_stmt` is the per-AST-node switch for statements. Helper emitters
// for lvalue references and default-value initialisation live here too.

#include "vm_compiler_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "c_backend_internal.h" /* for has_property (AST utility) */
#include "diag.h"
#include "ownership.h"
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
        if (!is_mod && expr->as.member.object &&
            expr->as.member.object->kind == AST_EXPR_IDENT &&
            compiler_find_local(compiler, expr->as.member.object->as.ident) < 0) {
            return compile_expr(compiler, expr);
        }
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
        // Automatically initialize to default value. OP_BIND_LOCAL frees
        // and replaces the slot directly without going through the
        // reference-aware OP_SET_LOCAL path — `let` is initialization,
        // not assignment-through-reference.
        if (!emit_default_value(compiler, stmt->as.let_stmt.type, (int)stmt->line)) {
          return false;
        }
        emit_op(compiler, OP_BIND_LOCAL, (int)stmt->line);
        emit_uint32(compiler, (uint32_t)slot, (int)stmt->line);
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
              // When the callee declared `ret view T` / `ret mod T` but
              // its body is `ret view <call>` over a native that returns
              // a value (e.g. componentView -> rae_ext_rae_buf_get), the
              // VM gets a real struct on the stack instead of a VAL_REF.
              // Detect this and wrap: stash the value in a hidden backing
              // slot, then push a REF to that slot so subsequent field/
              // method accesses through the binding see a proper REF.
              bool returns_ref_but_value = false;
              if (stmt->as.let_stmt.value->kind == AST_EXPR_CALL) {
                  const AstExpr* callee = stmt->as.let_stmt.value->as.call.callee;
                  if (callee->kind == AST_EXPR_IDENT) {
                      Str name = callee->as.ident;
                      FunctionEntry* entry = function_table_find(&compiler->compiler_ctx->functions, name);
                      if (entry && entry->returns_ref) {
                          already_ref = true;
                          if (!entry->is_extern) returns_ref_but_value = true;
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
                      if (!entry->is_extern) returns_ref_but_value = true;
                  }
              } else if (stmt->as.let_stmt.value->kind == AST_EXPR_NONE) {
                  already_ref = true;
              }

              if (!already_ref) {
                  diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value; RHS must be a reference or a function returning one");
                  return false;
              }

              if (returns_ref_but_value) {
                  // Allocate a hidden temp local to back the borrow,
                  // store the call result there, and replace the
                  // top-of-stack value with a REF to that temp.
                  char temp_name[64];
                  snprintf(temp_name, sizeof(temp_name), "__view_temp_%zu_%zu",
                           stmt->line, stmt->column);
                  int backing = compiler_add_local(
                      compiler, str_from_cstr(temp_name), type_name, false, false);
                  if (backing < 0) return false;
                  if (!compiler_ensure_local_capacity(
                          compiler, compiler->local_count, (int)stmt->line)) {
                      return false;
                  }
                  emit_op(compiler, OP_BIND_LOCAL, (int)stmt->line);
                  emit_uint32(compiler, (uint32_t)backing, (int)stmt->line);
                  bool want_mod = stmt->as.let_stmt.type && stmt->as.let_stmt.type->is_mod;
                  emit_op(compiler, want_mod ? OP_MOD_LOCAL : OP_VIEW_LOCAL, (int)stmt->line);
                  emit_uint32(compiler, (uint32_t)backing, (int)stmt->line);
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
          // `let x: T = expr` is initialization, not assignment-through-ref.
          //
          // We pick the binding opcode based on the let target's declared
          // type (this branch is reached only when the source form is `=`,
          // not `=>`; the `=>` path above keeps OP_BIND_LOCAL so the slot
          // intentionally holds a VAL_REF).
          //
          // Value-typed targets (primitives, enums) use OP_BIND_LOCAL_VALUE:
          // if the RHS happens to be a VAL_REF (e.g. `let n: Int = view_int_param`
          // where the call site emitted OP_VIEW_LOCAL), the VM derefs the
          // ref so the new local owns a fresh primitive value. That avoids
          // the read-only diagnostic on a subsequent reassignment.
          //
          // Non-value targets (structs, List, Map, String, etc.) keep
          // OP_BIND_LOCAL. Deref-copying a VAL_REF to a struct target via
          // value_copy() recurses through every field/element and is
          // catastrophically expensive on deeply nested data (mobile UI's
          // JsonValue/Scene graphs hung the Live VM at startup). Composite
          // targets that arrive as VAL_REF are already handled correctly
          // by downstream OP_SET_LOCAL / field-access opcodes, which
          // walk through the ref transparently.
          bool target_is_value =
              stmt->as.let_stmt.type &&
              !stmt->as.let_stmt.type->is_view &&
              !stmt->as.let_stmt.type->is_mod &&
              !stmt->as.let_stmt.type->is_opt &&
              vm_is_value_type(compiler->compiler_ctx, type_name);
          emit_op(compiler,
                  target_is_value ? OP_BIND_LOCAL_VALUE : OP_BIND_LOCAL,
                  (int)stmt->line);
          emit_uint32(compiler, (uint32_t)slot, (int)stmt->line);
      }

      /* Stage 1 step 3 + step 5 — flag this local for scope-exit
       * cleanup. Two paths converge here:
       *
       *   USER STRUCT  — needs a registered drop helper. Emit
       *                  OP_MOD_LOCAL + OP_NATIVE_CALL. Variant
       *                  (FULL/ALIAS) selected from binding-site.
       *                  Gate: non-generic, non-c_struct user
       *                  struct decl exists in ctx->all_decls.
       *   LEAF         — String, List(T), Buffer(T), StringMap(V),
       *                  IntMap(V). No helper needed; OP_DROP_LOCAL
       *                  just value_free's the slot. No FULL/ALIAS
       *                  distinction (leaves don't cascade through
       *                  fields, value_free already walks
       *                  containers fully).
       *
       * Both paths require: value-owning binding (not view/mod/opt),
       * not the `=>` bind form, and type cascade-droppable per the
       * backend-neutral predicate. */
      if (slot >= 0
          && stmt->as.let_stmt.type
          && !stmt->as.let_stmt.is_bind
          && !stmt->as.let_stmt.type->is_view
          && !stmt->as.let_stmt.type->is_mod
          && !stmt->as.let_stmt.type->is_opt
          && type_needs_cascade_drop(compiler->compiler_ctx,
                                     compiler->module,
                                     stmt->as.let_stmt.type, 0)) {
        Str base = get_base_type_name(stmt->as.let_stmt.type);
        bool is_leaf =
          str_eq_cstr(base, "String")
          || str_eq_cstr(base, "List")
          || str_eq_cstr(base, "Buffer")
          || str_eq_cstr(base, "StringMap")
          || str_eq_cstr(base, "IntMap");
        if (is_leaf) {
          compiler->locals[slot].needs_drop = true;
          compiler->locals[slot].is_leaf_drop = true;
          compiler->locals[slot].is_alias = false;
        } else {
          const AstDecl* tdecl = NULL;
          for (size_t i = 0; i < compiler->compiler_ctx->all_decl_count; ++i) {
            const AstDecl* d = compiler->compiler_ctx->all_decls[i];
            if (d->kind != AST_DECL_TYPE) continue;
            if (!str_eq(d->as.type_decl.name, base)) continue;
            tdecl = d; break;
          }
          bool helper_exists =
            tdecl != NULL
            && tdecl->as.type_decl.generic_params == NULL
            && !has_property(tdecl->as.type_decl.properties, "c_struct");
          if (helper_exists) {
            compiler->locals[slot].needs_drop = true;
            bool is_struct_literal_init =
              stmt->as.let_stmt.value
              && stmt->as.let_stmt.value->kind == AST_EXPR_OBJECT;
            bool is_default_init = stmt->as.let_stmt.value == NULL;
            compiler->locals[slot].is_alias =
              !(is_struct_literal_init || is_default_init);
          }
        }
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
              /* Stage 1 step 3 — drop every still-live owning local
               * before the void return. No exclude_slot: no value
               * is moving out. */
              if (!vm_emit_implicit_drops(compiler, /*min=*/0,
                                          /*exclude=*/-1,
                                          (int)stmt->line)) {
                return false;
              }
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
            /* Stage 1 step 3 — drop every live owning local before
             * the return. Move-tracking exception: when the return
             * value is a bare local identifier, skip dropping that
             * specific slot — its heap is flowing to the caller and
             * dropping here would either free it (caller use-after-
             * free) or double-free (caller drops it too). */
            int exclude_slot = -1;
            if (arg->value && arg->value->kind == AST_EXPR_IDENT) {
              exclude_slot = compiler_find_local(compiler, arg->value->as.ident);
            }
            if (!vm_emit_implicit_drops(compiler, /*min=*/0,
                                        exclude_slot,
                                        (int)stmt->line)) {
              return false;
            }
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
            /* Stage 1 step 4 — explicit cascade drop on the OLD
             * value before overwriting. Only emitted when the local
             * is `needs_drop` (set at the let binding site by the
             * same registered-helper gate the scope-exit pass
             * uses). FULL vs ALIAS comes from the binding site
             * (`is_alias`). Self-assignment safety: the RHS is
             * already fully evaluated and sitting on top of the
             * stack at this point, and OP_GET_LOCAL deep-copies on
             * read, so dropping the slot here cannot touch heap
             * that the new value still references.
             *
             * The runtime OP_SET_LOCAL also calls value_free on
             * the slot; that is now redundant but reentrant-safe
             * because our helper sets heap pointers to NULL. It
             * stays as a defense-in-depth net (and is what handles
             * leaf String/List/Map reassignment today — those
             * types deliberately don't get an explicit emission
             * in this commit; see commit message).
             *
             * After drop, mark the local as `dropped=true` so the
             * scope-exit / ret cleanup pass does not fire on this
             * slot again (the slot now contains the NEW value
             * which will be cleaned up on its OWN scope exit by
             * the slot's binding metadata when it re-flips
             * `dropped=false` … but here we keep it as-is: the
             * slot was reassigned and the NEW value's structural
             * ownership matches the OLD binding's classification,
             * so we DO want scope-exit to drop it again). Reset
             * back to false for the next cleanup pass. */
            if (compiler->locals[slot].needs_drop
                && !compiler->locals[slot].dropped) {
              if (compiler->locals[slot].is_leaf_drop) {
                /* Leaf reassign — still goes through the runtime
                 * OP_SET_LOCAL value_free path, but emit an
                 * explicit OP_DROP_LOCAL beforehand so the
                 * cleanup is visible in the bytecode and
                 * symmetric with scope-exit. Reentrant-safe:
                 * after OP_DROP_LOCAL the slot is VAL_NONE, so
                 * OP_SET_LOCAL's value_free on it is a no-op. */
                emit_op(compiler, OP_DROP_LOCAL, (int)stmt->line);
                emit_uint32(compiler, (uint32_t)slot, (int)stmt->line);
              } else {
                char buf[256];
                Str tn = compiler->locals[slot].type_name;
                int n = snprintf(buf, sizeof(buf),
                                 "rae_vm_drop_struct_%.*s%s",
                                 (int)tn.len, tn.data,
                                 compiler->locals[slot].is_alias
                                   ? "_alias" : "");
                if (n > 0 && n < (int)sizeof(buf)) {
                  emit_op(compiler, OP_MOD_LOCAL, (int)stmt->line);
                  emit_uint32(compiler, (uint32_t)slot, (int)stmt->line);
                  if (!emit_native_call(compiler, str_from_cstr(buf),
                                        1, (int)stmt->line, 0)) {
                    return false;
                  }
                  emit_op(compiler, OP_POP, (int)stmt->line);
                }
              }
            }
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
