#ifndef RAE_TYPE_H
#define RAE_TYPE_H

#include "str.h"
#include <stdbool.h>
#include <stdint.h>

// Forward declarations
struct AstDecl;
struct AstTypeRef;
struct CompilerContext;

typedef enum {
    TYPE_UNKNOWN,
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STRING,
    TYPE_CHAR,
    TYPE_STRUCT, // A concrete struct instance (possibly specialized)
    TYPE_GENERIC_INST, // A generic struct instantiation (e.g. List(Int))
    TYPE_REF, // view T or mod T
    TYPE_OPT, // opt T
    TYPE_FUNC, // Function type (for first-class functions)
    TYPE_ANY, // Type-erased Any
    TYPE_BUFFER, // Built-in Buffer(T)
    TYPE_GENERIC_PARAM // T, U, V inside a generic definition
} TypeKind;

typedef struct TypeInfo TypeInfo;

struct TypeInfo {
    TypeKind kind;
    Str name; // Canonical name (e.g. "List(Int)")
    
    // For efficient interning
    struct TypeInfo* next_interned; 

    union {
        struct {
            struct AstDecl* decl; // The struct declaration
            TypeInfo** generic_args; // Array of resolved generic arguments
            size_t generic_count;
        } structure;

        struct {
            TypeInfo* base;
            bool is_mod; // true = mod, false = view
        } ref;

        struct {
            TypeInfo* base;
        } opt;
        
        struct {
             TypeInfo* base;
        } buffer;

        struct {
             Str param_name; // e.g. "T"
        } generic_param;
    } as;
};

// Global registry for type interning
typedef struct SpecializationEntry {
    struct AstDecl* generic_decl;
    TypeInfo** generic_args;
    size_t arg_count;
    struct AstDecl* specialized_decl;
    struct SpecializationEntry* next;
} SpecializationEntry;

typedef struct TypeRegistry {
    TypeInfo** buckets;
    size_t capacity;
    size_t count;
    SpecializationEntry* specializations; // List of specialized declarations
    struct Arena* arena; // Allocator for TypeInfo structs
} TypeRegistry;

// Initialization
void type_registry_init(TypeRegistry* registry, struct Arena* arena);

// Specialization management
struct AstDecl* type_registry_find_specialization(TypeRegistry* r, struct AstDecl* generic_decl, TypeInfo** args, size_t arg_count);
void type_registry_add_specialization(TypeRegistry* r, struct AstDecl* generic_decl, TypeInfo** args, size_t arg_count, struct AstDecl* specialized_decl);

// Core Type Constructors (Interning included)
TypeInfo* type_get_void(TypeRegistry* registry);
TypeInfo* type_get_bool(TypeRegistry* registry);
TypeInfo* type_get_int(TypeRegistry* registry);
TypeInfo* type_get_float(TypeRegistry* registry);
TypeInfo* type_get_string(TypeRegistry* registry);
TypeInfo* type_get_char(TypeRegistry* registry);
TypeInfo* type_get_any(TypeRegistry* registry);

TypeInfo* type_get_ref(TypeRegistry* registry, TypeInfo* base, bool is_mod);
TypeInfo* type_get_opt(TypeRegistry* registry, TypeInfo* base);
TypeInfo* type_get_buffer(TypeRegistry* registry, TypeInfo* base);
TypeInfo* type_get_struct(TypeRegistry* registry, struct AstDecl* decl, TypeInfo** args, size_t arg_count);
TypeInfo* type_get_generic_param(TypeRegistry* registry, Str name);

// Utilities
bool type_is_same(TypeInfo* a, TypeInfo* b);
bool type_is_primitive(TypeInfo* t);
bool type_is_numeric(TypeInfo* t);
// Generates a C-safe mangled name for the type
Str type_mangle_name(struct Arena* arena, TypeInfo* t);

#endif // RAE_TYPE_H
