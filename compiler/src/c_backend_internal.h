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
  size_t local_count;
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

// -- Discovery pass --
void collect_type_refs_module(CompilerContext* ctx);
void discover_specializations_expr(CFuncContext* ctx, const AstExpr* expr);
void discover_specializations_stmt(CFuncContext* ctx, const AstStmt* stmt);
void discover_specializations_module(CompilerContext* ctx, const AstModule* module);

// -- Function emission (called from the orchestrator) --
bool emit_function(CompilerContext* compiler_ctx, const AstModule* module, const AstFuncDecl* func, FILE* out, const struct VmRegistry* registry, bool uses_raylib);
bool emit_specialized_function(CompilerContext* ctx, const AstModule* m, const AstFuncDecl* f, const AstTypeRef* args, FILE* out, const struct VmRegistry* r, bool ray);

#endif /* C_BACKEND_INTERNAL_H */
