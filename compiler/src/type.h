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
    TYPE_FLOAT,   // f32 / IEEE-754 binary32. `Float` and `Float32` both resolve here.
    TYPE_FLOAT64, // f64 / IEEE-754 binary64. Distinct type, never aliased by `Float`.
    TYPE_STRING,
    TYPE_CHAR,
    TYPE_STRUCT, // A concrete struct instance (possibly specialized)
    TYPE_GENERIC_INST, // A generic struct instantiation (e.g. List(Int))
    TYPE_REF, // view T or mod T
    TYPE_OPT, // opt T
    TYPE_FUNC, // Function type (for first-class functions)
    TYPE_ANY, // Type-erased Any
    TYPE_BUFFER, // Built-in Buffer(T)
    TYPE_TASK, // Built-in Task(T) — a concurrent task handle
    TYPE_GENERIC_PARAM, // T, U, V inside a generic definition

    /* A compile-time Int appearing in a generic ARGUMENT position, e.g. the
     * 16 in `Array(Float, cap: 16)`.
     *
     * Modelled as a TypeInfo on purpose. Generic identity throughout the
     * compiler is interned-POINTER identity — `type_is_same` is `a == b`, and
     * `type_registry_find_specialization` compares a `TypeInfo*[]` element by
     * element. Interning the value here means monomorphization, specialization
     * caching and name mangling all keep working unchanged, instead of every
     * one of them needing a parallel "and also these value arguments" path.
     *
     * Int only, deliberately. Counts are the only value arguments with a
     * principled use today; Bool/Float/String const arguments would be
     * generality without a language-level justification. */
    TYPE_CONST_INT,

    /* Array(T, cap: N) — a fixed-size, contiguous, BY-VALUE aggregate.
     * Distinct from TYPE_BUFFER, which is a bare `T*` with no length in the
     * type. See docs/value-aggregates-and-ownership.md §1.3. */
    TYPE_ARRAY,

    /* Sentinel: one past the last valid kind. Range checks must use this
     * rather than naming whichever kind happens to be last today — an
     * appended kind silently fell outside a hard-coded bound once already
     * and was mangled as "rae_unresolved". Keep it last. */
    TYPE__COUNT
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
             TypeInfo* base; // result type T of Task(T)
        } task;

        struct {
             Str param_name; // e.g. "T"
        } generic_param;

        struct {
             int64_t value;
        } const_int;

        /* Fixed-width integer metadata for TYPE_INT. The canonical `Int`
         * (and `Int64`) is bits=64, is_unsigned=false. Int8/16/32,
         * UInt8/16/32/64 carry their real width/signedness so they intern
         * distinctly, mangle to the right C type (int32_t, uint16_t, …) and
         * give a byte-accurate layout for `List(Int32)` etc. (#507). Zero-
         * initialised memory reads as {0,false} = a malformed 0-bit int, so
         * always construct TYPE_INT through type_get_int/type_get_int_sized. */
        struct {
             int bits;         // 8, 16, 32, 64
             bool is_unsigned; // false = signed
        } integer;

        struct {
             TypeInfo* base;  // element type
             int64_t count;   // element count, >= 0, known at compile time
        } array;
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
TypeInfo* type_get_int(TypeRegistry* registry);      // canonical Int (64-bit signed)
/* Fixed-width integer (#507). bits in {8,16,32,64}. (64,false) is the canonical
 * Int and returns the same interned TypeInfo as type_get_int. */
TypeInfo* type_get_int_sized(TypeRegistry* registry, int bits, bool is_unsigned);
/* C spelling for a fixed-width integer, e.g. (32,true) -> "uint32_t". */
const char* rae_int_c_name(int bits, bool is_unsigned);
TypeInfo* type_get_float(TypeRegistry* registry);   // Float == Float32 (f32)
TypeInfo* type_get_float64(TypeRegistry* registry); // Float64 (f64)
TypeInfo* type_get_string(TypeRegistry* registry);
TypeInfo* type_get_char(TypeRegistry* registry);
TypeInfo* type_get_any(TypeRegistry* registry);

TypeInfo* type_get_ref(TypeRegistry* registry, TypeInfo* base, bool is_mod);
TypeInfo* type_get_opt(TypeRegistry* registry, TypeInfo* base);
TypeInfo* type_get_buffer(TypeRegistry* registry, TypeInfo* base);
TypeInfo* type_get_task(TypeRegistry* registry, TypeInfo* base);
TypeInfo* type_get_const_int(TypeRegistry* registry, int64_t value);
TypeInfo* type_get_array(TypeRegistry* registry, TypeInfo* base, int64_t count);
TypeInfo* type_get_struct(TypeRegistry* registry, struct AstDecl* decl, TypeInfo** args, size_t arg_count);
TypeInfo* type_get_generic_param(TypeRegistry* registry, Str name);

// Utilities
bool type_is_same(TypeInfo* a, TypeInfo* b);
bool type_is_primitive(TypeInfo* t);
bool type_is_numeric(TypeInfo* t);
// Generates a C-safe mangled name for the type
Str type_mangle_name(struct Arena* arena, TypeInfo* t);

#endif // RAE_TYPE_H
