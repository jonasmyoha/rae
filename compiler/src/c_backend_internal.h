#ifndef C_BACKEND_INTERNAL_H
#define C_BACKEND_INTERNAL_H

// Internal header shared across the c_backend_*.c modules. Anything declared
// here is accessible from any of those files; anything kept static within a
// .c file stays private to it. Public surface (callable from outside the
// c_backend) lives in `c_backend.h`.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ast.h"

struct VmRegistry;

// -- Precedence levels for C expressions (mirrors C operator precedence) --
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
    const char** items;
    size_t count;
    size_t capacity;
} EmittedTypeList;

typedef struct {
  CompilerContext* compiler_ctx;
  const AstModule* module;
  const AstFuncDecl* func_decl;
  const AstParam* params;
  const AstIdentifierPart* generic_params;
  const AstTypeRef* generic_args;
  const char* return_type_name;
  Str locals[256];
  Str local_types[256];
  const AstTypeRef* local_type_refs[256];
  bool local_is_ptr[256];
  bool local_is_mod[256];
  // Stage 3 move tracking (docs/ownership-model.md). Set true when a
  // local's value flows into an ownership-consuming context: `own x`
  // expression, argument bound to an `own T` parameter, or returned
  // from the function. Read by emit_implicit_drops_for_body to skip
  // the auto-drop emission.
  bool local_moved[256];
  // Layer 5 (docs/scope-exit-dealloc.md) ownership gate: true when
  // a user-struct let was constructed in place (struct literal or
  // auto-init) and so genuinely owns its heap. False for call-result
  // bindings — even if the callee returns plain `T`, our runtime
  // doesn't deep-copy structs, so the binding's heap pointers alias
  // someone else's storage and auto-drop would double-free.
  //
  // Per the language design (docs/ownership-model.md) plain `T`
  // returns SHOULD own — this gate is a transitional measure until
  // stdlib APIs that currently return shallow aliases (sceneNodeAt,
  // componentGet on heap-owning T, JsonDoc helpers, ...) get
  // migrated to `view T` returns. Once they're migrated, this gate
  // becomes unnecessary and Layer 5 can fire on all owning struct
  // lets unconditionally.
  bool local_struct_owns_heap[256];
  size_t local_count;
  // Stage 7 early-return cleanup: the index in `locals[]` after which
  // entries are this function's `let` bindings (i.e. NOT parameters).
  // emit_function records this right after the parameter list; the
  // ret-stmt epilogue uses it to know which range of locals to drop
  // before each return. Set to (size_t)-1 when not inside a function
  // body emit.
  size_t func_first_let_idx;
  bool returns_value;
  size_t temp_counter;
  AstTypeRef expected_type;
  bool has_expected_type;
  bool suppress_opt_unbox; // when true, skip emit_opt_unbox_suffix (e.g. inside `... is none`)
  const struct VmRegistry* registry;
  bool uses_raylib;
  bool is_main;
  int scope_depth;
  CDeferStack defer_stack;
} CFuncContext;

// -- Helpers (small, used widely) --
bool emitted_list_contains(EmittedTypeList* list, const char* name);
void emitted_list_add(EmittedTypeList* list, const char* name);
bool types_match(Str a, Str b);
bool is_primitive_ref(CFuncContext* ctx, const AstTypeRef* tr);
bool is_pointer_type(CFuncContext* ctx, Str name);
bool is_generic_param(const AstIdentifierPart* params, Str name);
bool has_property(const AstProperty* props, const char* name);
int binary_op_precedence(AstBinaryOp op);
Str get_local_type_name(CFuncContext* ctx, Str name);
const AstTypeRef* get_local_type_ref(CFuncContext* ctx, Str name);
bool func_has_return_value(const AstFuncDecl* func);

// -- Generic-call type-arg hoisting (shared by discovery + emission) --
// `createList(String, initialCap: 4)` / `createList(type: String, …)` /
// `String.createList(…)` all park `String` in the regular arg list;
// these helpers move it into `expr->as.call.generic_args`.
AstTypeRef* try_as_type_arg(CFuncContext* ctx, const AstExpr* val);
AstExpr* hoist_type_arg_if_present(CFuncContext* ctx, const AstExpr* expr);

// -- Decl lookup --
const AstDecl* find_type_decl(CFuncContext* ctx, const AstModule* module, Str name);
const AstDecl* find_enum_decl(CFuncContext* ctx, const AstModule* module, Str name);
const AstFuncDecl* find_function_overload(const AstModule* module, CFuncContext* ctx, Str name, const Str* param_types, uint16_t param_count, bool is_method, const AstExpr* call_expr);

// -- Type inference (peeking at expression types without emitting) --
const AstTypeRef* infer_expr_type_ref(CFuncContext* ctx, const AstExpr* expr);
Str infer_expr_type(CFuncContext* ctx, const AstExpr* expr);

// -- Type emission --
bool emit_type_ref_as_c_type(CFuncContext* ctx, const AstTypeRef* type, FILE* out, bool skip_ptr);
void emit_type_info_as_c_type(CFuncContext* ctx, TypeInfo* t, FILE* out);
bool emit_param_list(CFuncContext* ctx, const AstParam* params, FILE* out, bool is_extern);
const char* c_return_type(CFuncContext* ctx, const AstFuncDecl* func);
bool emit_string_literal(FILE* out, Str literal);
bool emit_auto_init(CFuncContext* ctx, const AstTypeRef* type, FILE* out);
bool emit_struct_auto_init(CFuncContext* ctx, const AstDecl* decl, const AstTypeRef* tr, FILE* out);
bool emit_type_recursive(CompilerContext* ctx, const AstModule* m, const AstTypeRef* type, FILE* out, EmittedTypeList* emitted, EmittedTypeList* visiting, bool ray);

// -- Decl/spec registry --
void register_decl(CompilerContext* ctx, const AstDecl* decl);
void collect_decls_from_module(CompilerContext* ctx, const AstModule* module);
bool type_refs_equal(const AstTypeRef* a, const AstTypeRef* b);
bool is_concrete_type(const AstTypeRef* type);

// -- Expression / statement / call emission entry points --
bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out, int parent_prec, bool is_lvalue, bool suppress_deref);
bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
bool emit_call_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out);
void emit_opt_unbox_suffix(CFuncContext* ctx, const AstFuncDecl* fd, const AstTypeRef* call_concrete, FILE* out);

// -- Defer stack --
bool emit_defers(CFuncContext* ctx, int min_depth, FILE* out);
void pop_defers(CFuncContext* ctx, int depth);

// -- Scope-exit dealloc (Stage 2; see docs/scope-exit-dealloc.md) --
// Emit `drop()` calls for heap-owning lets in [first_let_index, local_count).
// Anything before first_let_index is a parameter (owned by the caller) and
// must be skipped.
bool emit_implicit_drops_for_body(CFuncContext* ctx, FILE* out,
                                  size_t first_let_index);
// Stage C: emit cascade drops for `own T` parameters in [0, first_let_index).
// Move-tracking skips drops for params that were returned or transferred.
bool emit_implicit_drops_for_own_params(CFuncContext* ctx, FILE* out,
                                        size_t first_let_index);
// Predicate used by both the emit pass and the discovery pass.
bool is_drop_target_type(const AstTypeRef* type);
// Find the `drop` overload whose receiver-base matches `container_base`.
const AstFuncDecl* find_drop_overload_for(CFuncContext* ctx, Str container_base);
// Layer 5 (docs/scope-exit-dealloc.md): does the type transitively own
// heap storage? Returns true for List/StringMap/IntMap directly, and
// for user structs with at least one heap-owning field.
bool type_owns_heap_storage(CompilerContext* cctx, const AstModule* module,
                            const AstTypeRef* type, int depth);
// Permissive variant — also considers String fields heap-needing.
// Used by cascade-drop sites only (Layer 5 struct synthesis, List/Map
// element drop). NOT by local auto-drop, which uses the strict form
// above to avoid the shallow-alias double-free pattern.
bool type_needs_cascade_drop(CompilerContext* cctx, const AstModule* module,
                             const AstTypeRef* type, int depth);
// Move tracking helpers (Stage 3 of docs/ownership-model.md).
void mark_local_moved_by_name(CFuncContext* ctx, Str name);
void mark_expr_moved_if_local(CFuncContext* ctx, const AstExpr* expr);

// -- Discovery pass --
void collect_type_refs_module(CompilerContext* ctx);
void discover_specializations_expr(CFuncContext* ctx, const AstExpr* expr);
void discover_specializations_stmt(CFuncContext* ctx, const AstStmt* stmt);
void discover_specializations_module(CompilerContext* ctx, const AstModule* module);

// -- Function emission (called from the orchestrator) --
bool emit_function(CompilerContext* compiler_ctx, const AstModule* module, const AstFuncDecl* func, FILE* out, const struct VmRegistry* registry, bool uses_raylib);
bool emit_specialized_function(CompilerContext* ctx, const AstModule* m, const AstFuncDecl* f, const AstTypeRef* args, FILE* out, const struct VmRegistry* r, bool ray);

#endif /* C_BACKEND_INTERNAL_H */
