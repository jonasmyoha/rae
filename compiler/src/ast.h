/* ast.h - Rae abstract syntax tree definitions */

#ifndef AST_H
#define AST_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#include "str.h"

typedef struct AstDecl AstDecl;
typedef struct AstBlock AstBlock;
typedef struct AstStmt AstStmt;
typedef struct AstExpr AstExpr;
typedef struct AstTypeRef AstTypeRef;

typedef struct AstIdentifierPart {
  Str text;
  struct AstIdentifierPart* next;
} AstIdentifierPart;

struct AstTypeRef {
  AstIdentifierPart* parts;
};

typedef struct AstProperty {
  Str name;
  struct AstProperty* next;
} AstProperty;

typedef struct AstParam {
  Str name;
  AstTypeRef* type;
  struct AstParam* next;
} AstParam;

typedef struct AstReturnItem {
  bool has_name;
  Str name;
  AstTypeRef* type;
  struct AstReturnItem* next;
} AstReturnItem;

typedef struct AstTypeField {
  Str name;
  AstTypeRef* type;
  struct AstTypeField* next;
} AstTypeField;

typedef struct AstCallArg {
  Str name;
  AstExpr* value;
  struct AstCallArg* next;
} AstCallArg;

typedef struct AstObjectField {
  Str name;
  AstExpr* value;
  struct AstObjectField* next;
} AstObjectField;

typedef struct AstDestructureBinding {
  Str local_name;
  Str return_label;
  struct AstDestructureBinding* next;
} AstDestructureBinding;

typedef enum {
  AST_BIN_ADD,
  AST_BIN_SUB,
  AST_BIN_MUL,
  AST_BIN_DIV,
  AST_BIN_MOD,
  AST_BIN_LT,
  AST_BIN_GT,
  AST_BIN_LE,
  AST_BIN_GE,
  AST_BIN_IS,
  AST_BIN_AND,
  AST_BIN_OR
} AstBinaryOp;

typedef enum {
  AST_UNARY_NEG,
  AST_UNARY_NOT,
  AST_UNARY_SPAWN
} AstUnaryOp;

typedef enum {
  AST_EXPR_IDENT,
  AST_EXPR_INTEGER,
  AST_EXPR_STRING,
  AST_EXPR_BOOL,
  AST_EXPR_NONE,
  AST_EXPR_BINARY,
  AST_EXPR_UNARY,
  AST_EXPR_CALL,
  AST_EXPR_MEMBER,
  AST_EXPR_OBJECT
} AstExprKind;

struct AstExpr {
  AstExprKind kind;
  size_t line;
  size_t column;
  union {
    Str ident;
    Str integer;
    Str string_lit;
    bool boolean;
    struct {
      AstExpr* lhs;
      AstExpr* rhs;
      AstBinaryOp op;
    } binary;
    struct {
      AstExpr* operand;
      AstUnaryOp op;
    } unary;
    struct {
      AstExpr* callee;
      AstCallArg* args;
    } call;
    struct {
      AstExpr* object;
      Str member;
    } member;
    AstObjectField* object;
  } as;
};

typedef struct AstReturnArg {
  bool has_label;
  Str label;
  AstExpr* value;
  struct AstReturnArg* next;
} AstReturnArg;

typedef enum {
  AST_STMT_DEF,
  AST_STMT_DESTRUCT,
  AST_STMT_EXPR,
  AST_STMT_RET,
  AST_STMT_IF,
  AST_STMT_MATCH
} AstStmtKind;

typedef struct AstMatchCase {
  AstExpr* pattern;
  AstBlock* block;
  struct AstMatchCase* next;
} AstMatchCase;

struct AstStmt {
  AstStmtKind kind;
  size_t line;
  size_t column;
  struct AstStmt* next;
  union {
    struct {
      Str name;
      AstTypeRef* type;
      bool is_move;
      AstExpr* value;
    } def_stmt;
    struct {
      AstDestructureBinding* bindings;
      AstExpr* call;
    } destruct_stmt;
    AstExpr* expr_stmt;
    struct {
      AstReturnArg* values;
    } ret_stmt;
    struct {
      AstExpr* condition;
      AstBlock* then_block;
      AstBlock* else_block;
    } if_stmt;
    struct {
      AstExpr* subject;
      AstMatchCase* cases;
    } match_stmt;
  } as;
};

struct AstBlock {
  AstStmt* first;
};

typedef enum {
  AST_DECL_TYPE,
  AST_DECL_FUNC
} AstDeclKind;

typedef struct {
  Str name;
  AstProperty* properties;
  AstTypeField* fields;
} AstTypeDecl;

typedef struct {
  Str name;
  AstParam* params;
  AstProperty* properties;
  AstReturnItem* returns;
  AstBlock* body;
} AstFuncDecl;

struct AstDecl {
  AstDeclKind kind;
  size_t line;
  size_t column;
  AstDecl* next;
  union {
    AstTypeDecl type_decl;
    AstFuncDecl func_decl;
  } as;
};

typedef struct {
  AstDecl* decls;
} AstModule;

void ast_dump_module(const AstModule* module, FILE* out);

#endif /* AST_H */
