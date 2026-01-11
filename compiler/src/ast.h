/* ast.h - Rae abstract syntax tree definitions */

#ifndef AST_H
#define AST_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#include "arena.h"
#include "str.h"
#include "lexer.h"

typedef struct AstDecl AstDecl;
typedef struct AstBlock AstBlock;
typedef struct AstStmt AstStmt;
typedef struct AstExpr AstExpr;
typedef struct AstTypeRef AstTypeRef;
typedef struct AstImport AstImport;
typedef struct AstMatchArm AstMatchArm;

typedef struct AstIdentifierPart {
  Str text;
  struct AstIdentifierPart* next;
} AstIdentifierPart;

typedef struct AstTypeRef {
  AstIdentifierPart* parts;
  bool is_opt;
  bool is_view;
  bool is_mod;
  bool is_id;
  bool is_key;
  size_t line;
  size_t column;
  struct AstTypeRef* generic_args; // Head of a linked list of AstTypeRef for generic arguments
  struct AstTypeRef* next; // For linking AstTypeRef nodes in a list (e.g., generic_args list)
} AstTypeRef;


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
  AST_UNARY_SPAWN,
  AST_UNARY_PRE_INC,
  AST_UNARY_PRE_DEC,
  AST_UNARY_POST_INC,
  AST_UNARY_POST_DEC,
  AST_UNARY_VIEW,
  AST_UNARY_MOD
} AstUnaryOp;

typedef enum {
  AST_EXPR_IDENT,
  AST_EXPR_INTEGER,
  AST_EXPR_FLOAT,
  AST_EXPR_STRING,
  AST_EXPR_CHAR,
  AST_EXPR_BOOL,
  AST_EXPR_NONE,
  AST_EXPR_BINARY,
  AST_EXPR_UNARY,
  AST_EXPR_CALL,
  AST_EXPR_MEMBER,
  AST_EXPR_OBJECT, // This now maps to AstObjectLiteral
  AST_EXPR_MATCH,
  AST_EXPR_LIST,
  AST_EXPR_INDEX,
  AST_EXPR_METHOD_CALL,
  AST_EXPR_COLLECTION_LITERAL,
  AST_EXPR_INTERP
} AstExprKind;

typedef struct AstInterpPart {
  AstExpr* value; // STRING or any other expression
  struct AstInterpPart* next;
} AstInterpPart;

typedef struct AstExprList {
  AstExpr* value;
  struct AstExprList* next;
} AstExprList;

typedef struct AstCollectionElement {
  Str* key; // Null if it's a list element
  AstExpr* value;
  struct AstCollectionElement* next;
} AstCollectionElement;

typedef struct AstCollectionLiteral {
  AstTypeRef* type; // Optional type hint (e.g. List[Int])
  AstCollectionElement* elements;
} AstCollectionLiteral;

// New struct definition (moved outside AstExpr)
typedef struct AstObjectLiteral {
  AstTypeRef* type; // Optional: The type of the object literal (e.g., "Point")
  AstObjectField* fields;
} AstObjectLiteral;

struct AstExpr {
  AstExprKind kind;
  size_t line;
  size_t column;
  bool is_raw;
  union {
    Str ident;
    Str integer;
    Str floating;
    Str string_lit;
    Str char_lit;
    int64_t char_value;
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
    AstObjectLiteral object_literal; // Updated to use the new struct
    struct {
      AstExpr* subject;
      AstMatchArm* arms;
    } match_expr;
    AstExprList* list;
    struct {
      AstExpr* target;
      AstExpr* index;
    } index;
    struct {
      AstExpr* object;
      Str method_name;
      AstCallArg* args;
    } method_call;
    AstCollectionLiteral collection;
    struct {
      AstInterpPart* parts;
    } interp;
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
  AST_STMT_LOOP,
  AST_STMT_MATCH,
  AST_STMT_ASSIGN
} AstStmtKind;

typedef struct AstMatchCase {
  AstExpr* pattern;
  AstBlock* block;
  struct AstMatchCase* next;
} AstMatchCase;

typedef struct AstMatchArm {
  AstExpr* pattern;
  AstExpr* value;
  struct AstMatchArm* next;
} AstMatchArm;

struct AstStmt {
  AstStmtKind kind;
  size_t line;
  size_t column;
  struct AstStmt* next;
  union {
    struct {
      Str name;
      AstTypeRef* type;
      bool is_bind;
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
      AstStmt* init;
      AstExpr* condition;
      AstExpr* increment;
      AstBlock* body;
      bool is_range;
    } loop_stmt;
    struct {
      AstExpr* subject;
      AstMatchCase* cases;
    } match_stmt;
    struct {
      AstExpr* target;
      AstExpr* value;
      bool is_bind;
    } assign_stmt;
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
  AstIdentifierPart* generic_params; // Linked list of type parameter names (e.g. T, K, V)
  AstTypeField* fields;
} AstTypeDecl;

typedef struct {
  Str name;
  AstParam* params;
  AstIdentifierPart* generic_params; // Linked list of type parameter names (e.g. T, K, V)
  AstProperty* properties;
  AstReturnItem* returns;
  bool is_extern;
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

struct AstImport {
  bool is_export;
  Str path;
  size_t line;
  size_t column;
  AstImport* next;
};

typedef struct {
  AstImport* imports;
  AstDecl* decls;
  Token* comments;
  size_t comment_count;
} AstModule;

void ast_dump_module(const AstModule* module, FILE* out);

#endif /* AST_H */
