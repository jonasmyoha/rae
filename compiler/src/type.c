#include "type.h"
#include "arena.h"
#include "ast.h"
#include <string.h>
#include <stdlib.h>

#define TYPE_BUCKETS_INITIAL 256

void type_registry_init(TypeRegistry* registry, Arena* arena) {
    registry->arena = arena;
    registry->capacity = TYPE_BUCKETS_INITIAL;
    registry->count = 0;
    registry->buckets = (TypeInfo**)arena_alloc(arena, sizeof(TypeInfo*) * registry->capacity);
    memset(registry->buckets, 0, sizeof(TypeInfo*) * registry->capacity);
    registry->specializations = NULL;
}

AstDecl* type_registry_find_specialization(TypeRegistry* r, AstDecl* generic_decl, TypeInfo** args, size_t arg_count) {
    SpecializationEntry* curr = r->specializations;
    while (curr) {
        if (curr->generic_decl == generic_decl && curr->arg_count == arg_count) {
            bool match = true;
            for (size_t i = 0; i < arg_count; i++) {
                if (curr->generic_args[i] != args[i]) { match = false; break; }
            }
            if (match) return curr->specialized_decl;
        }
        curr = curr->next;
    }
    return NULL;
}

void type_registry_add_specialization(TypeRegistry* r, AstDecl* generic_decl, TypeInfo** args, size_t arg_count, AstDecl* specialized_decl) {
    SpecializationEntry* entry = arena_alloc(r->arena, sizeof(SpecializationEntry));
    entry->generic_decl = generic_decl;
    entry->arg_count = arg_count;
    if (arg_count > 0) {
        entry->generic_args = arena_alloc(r->arena, sizeof(TypeInfo*) * arg_count);
        memcpy(entry->generic_args, args, sizeof(TypeInfo*) * arg_count);
    } else {
        entry->generic_args = NULL;
    }
    entry->specialized_decl = specialized_decl;
    entry->next = r->specializations;
    r->specializations = entry;
}

// FNV-1a hash function for type structure
static uint64_t hash_type(TypeKind kind, const void* key_data, size_t key_len) {
    uint64_t hash = 14695981039346656037ULL;
    hash ^= (uint64_t)kind;
    hash *= 1099511628211ULL;
    
    const uint8_t* bytes = (const uint8_t*)key_data;
    for (size_t i = 0; i < key_len; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static TypeInfo* find_interned(TypeRegistry* reg, uint64_t hash, TypeKind kind, const void* key_data, size_t key_len) {
    size_t idx = hash % reg->capacity;
    TypeInfo* curr = reg->buckets[idx];
    while (curr) {
        if (curr->kind == kind) {
            // Simplified check: usually we'd store the key too, but here we reconstruct or check
            // For now, let's assume hash collision is rare enough or handled by deeper equality checks
            // But strict interning needs exact match.
            // Let's implement exact match logic based on kind.
            bool match = false;
            switch (kind) {
                case TYPE_VOID: case TYPE_BOOL: case TYPE_INT: case TYPE_FLOAT: 
                case TYPE_STRING: case TYPE_CHAR: case TYPE_ANY:
                    match = true; // Singletons
                    break;
                case TYPE_REF:
                    if (curr->as.ref.is_mod == ((bool*)key_data)[0] && 
                        curr->as.ref.base == ((TypeInfo**)key_data)[1]) match = true;
                    break;
                case TYPE_OPT:
                case TYPE_BUFFER:
                    if (curr->as.opt.base == *(TypeInfo**)key_data) match = true;
                    break;
                case TYPE_GENERIC_PARAM:
                    if (str_eq(curr->as.generic_param.param_name, *(Str*)key_data)) match = true;
                    break;
                 // Structs need more complex key data (decl pointer + args array)
                 default: break;
            }
            if (match) return curr;
        }
        curr = curr->next_interned;
    }
    return NULL;
}

static void add_interned(TypeRegistry* reg, uint64_t hash, TypeInfo* type) {
    size_t idx = hash % reg->capacity;
    type->next_interned = reg->buckets[idx];
    reg->buckets[idx] = type;
    reg->count++;
    // TODO: Resize if load factor high
}

// --- Primitive Constructors ---

TypeInfo* type_get_void(TypeRegistry* r) {
    static TypeInfo* t = NULL;
    if (!t) {
        t = (TypeInfo*)arena_alloc(r->arena, sizeof(TypeInfo));
        t->kind = TYPE_VOID;
        t->name = (Str){(uint8_t*)"Void", 4};
        add_interned(r, hash_type(TYPE_VOID, NULL, 0), t);
    }
    return t;
}

TypeInfo* type_get_bool(TypeRegistry* r) {
    static TypeInfo* t = NULL;
    if (!t) {
        t = (TypeInfo*)arena_alloc(r->arena, sizeof(TypeInfo));
        t->kind = TYPE_BOOL;
        t->name = (Str){(uint8_t*)"Bool", 4};
        add_interned(r, hash_type(TYPE_BOOL, NULL, 0), t);
    }
    return t;
}

TypeInfo* type_get_int(TypeRegistry* r) {
    static TypeInfo* t = NULL;
    if (!t) {
        t = (TypeInfo*)arena_alloc(r->arena, sizeof(TypeInfo));
        t->kind = TYPE_INT;
        t->name = (Str){(uint8_t*)"Int", 3};
        add_interned(r, hash_type(TYPE_INT, NULL, 0), t);
    }
    return t;
}

TypeInfo* type_get_float(TypeRegistry* r) {
    static TypeInfo* t = NULL;
    if (!t) {
        t = (TypeInfo*)arena_alloc(r->arena, sizeof(TypeInfo));
        t->kind = TYPE_FLOAT;
        t->name = (Str){(uint8_t*)"Float", 5};
        add_interned(r, hash_type(TYPE_FLOAT, NULL, 0), t);
    }
    return t;
}

TypeInfo* type_get_string(TypeRegistry* r) {
    static TypeInfo* t = NULL;
    if (!t) {
        t = (TypeInfo*)arena_alloc(r->arena, sizeof(TypeInfo));
        t->kind = TYPE_STRING;
        t->name = (Str){(uint8_t*)"String", 6};
        add_interned(r, hash_type(TYPE_STRING, NULL, 0), t);
    }
    return t;
}

TypeInfo* type_get_char(TypeRegistry* r) {
    static TypeInfo* t = NULL;
    if (!t) {
        t = (TypeInfo*)arena_alloc(r->arena, sizeof(TypeInfo));
        t->kind = TYPE_CHAR;
        t->name = (Str){(uint8_t*)"Char", 4};
        add_interned(r, hash_type(TYPE_CHAR, NULL, 0), t);
    }
    return t;
}

TypeInfo* type_get_any(TypeRegistry* r) {
    static TypeInfo* t = NULL;
    if (!t) {
        t = (TypeInfo*)arena_alloc(r->arena, sizeof(TypeInfo));
        t->kind = TYPE_ANY;
        t->name = (Str){(uint8_t*)"Any", 3};
        add_interned(r, hash_type(TYPE_ANY, NULL, 0), t);
    }
    return t;
}

TypeInfo* type_get_ref(TypeRegistry* r, TypeInfo* base, bool is_mod) {
    // Key: [is_mod (1 byte), base_ptr (8 bytes)]
    struct { bool m; TypeInfo* b; } key = { is_mod, base };
    uint64_t h = hash_type(TYPE_REF, &key, sizeof(key));
    
    // We need a custom find because the generic one above was illustrative.
    // Ideally we make `find_interned` generic or use a specific lookup here.
    // For speed implementing specialized logic inline:
    size_t idx = h % r->capacity;
    TypeInfo* curr = r->buckets[idx];
    while (curr) {
        if (curr->kind == TYPE_REF && curr->as.ref.is_mod == is_mod && curr->as.ref.base == base)
            return curr;
        curr = curr->next_interned;
    }

    TypeInfo* t = (TypeInfo*)arena_alloc(r->arena, sizeof(TypeInfo));
    t->kind = TYPE_REF;
    t->as.ref.base = base;
    t->as.ref.is_mod = is_mod;
    // Construct name: "view T" or "mod T"
    // Note: This leaks/uses arena for string construction, which is fine for interned types
    // Simplified name construction:
    // char buf[64]; snprintf(buf, 64, "%s %.*s", is_mod ? "mod" : "view", (int)base->name.len, base->name.data);
    // t->name = str_dup_arena(r->arena, (Str){(uint8_t*)buf, strlen(buf)});
    t->name = (Str){0}; // Defer name gen for now
    add_interned(r, h, t);
    return t;
}

TypeInfo* type_get_opt(TypeRegistry* r, TypeInfo* base) {
    uint64_t h = hash_type(TYPE_OPT, &base, sizeof(base));
    size_t idx = h % r->capacity;
    TypeInfo* curr = r->buckets[idx];
    while (curr) {
        if (curr->kind == TYPE_OPT && curr->as.opt.base == base) return curr;
        curr = curr->next_interned;
    }
    
    TypeInfo* t = (TypeInfo*)arena_alloc(r->arena, sizeof(TypeInfo));
    t->kind = TYPE_OPT;
    t->as.opt.base = base;
    add_interned(r, h, t);
    return t;
}

TypeInfo* type_get_buffer(TypeRegistry* r, TypeInfo* base) {
    uint64_t h = hash_type(TYPE_BUFFER, &base, sizeof(base));
    size_t idx = h % r->capacity;
    TypeInfo* curr = r->buckets[idx];
    while (curr) {
        if (curr->kind == TYPE_BUFFER && curr->as.buffer.base == base) return curr;
        curr = curr->next_interned;
    }

    TypeInfo* t = (TypeInfo*)arena_alloc(r->arena, sizeof(TypeInfo));
    t->kind = TYPE_BUFFER;
    t->as.buffer.base = base;
    add_interned(r, h, t);
    return t;
}

TypeInfo* type_get_generic_param(TypeRegistry* r, Str name) {
    uint64_t h = hash_type(TYPE_GENERIC_PARAM, name.data, name.len);
    size_t idx = h % r->capacity;
    TypeInfo* curr = r->buckets[idx];
    while (curr) {
        if (curr->kind == TYPE_GENERIC_PARAM && str_eq(curr->as.generic_param.param_name, name)) return curr;
        curr = curr->next_interned;
    }
    TypeInfo* t = (TypeInfo*)arena_alloc(r->arena, sizeof(TypeInfo));
    t->kind = TYPE_GENERIC_PARAM;
    t->as.generic_param.param_name = name;
    t->name = name;
    add_interned(r, h, t);
    return t;
}

TypeInfo* type_get_struct(TypeRegistry* r, AstDecl* decl, TypeInfo** args, size_t arg_count) {
    // Hash based on decl pointer and all generic args
    uint64_t h = hash_type(TYPE_STRUCT, &decl, sizeof(decl));
    for (size_t i = 0; i < arg_count; i++) {
        h ^= hash_type(TYPE_STRUCT, &args[i], sizeof(TypeInfo*));
    }

    size_t idx = h % r->capacity;
    TypeInfo* curr = r->buckets[idx];
    while (curr) {
        if (curr->kind == TYPE_STRUCT && curr->as.structure.decl == decl && curr->as.structure.generic_count == arg_count) {
            bool match = true;
            for (size_t i = 0; i < arg_count; i++) {
                if (curr->as.structure.generic_args[i] != args[i]) { match = false; break; }
            }
            if (match) return curr;
        }
        curr = curr->next_interned;
    }

    TypeInfo* t = (TypeInfo*)arena_alloc(r->arena, sizeof(TypeInfo));
    t->kind = TYPE_STRUCT;
    t->as.structure.decl = decl;
    t->as.structure.generic_count = arg_count;
    if (arg_count > 0) {
        t->as.structure.generic_args = (TypeInfo**)arena_alloc(r->arena, sizeof(TypeInfo*) * arg_count);
        memcpy(t->as.structure.generic_args, args, sizeof(TypeInfo*) * arg_count);
    } else {
        t->as.structure.generic_args = NULL;
    }
    
    // Canonical name: "Name(Arg1, Arg2)"
    t->name = decl->as.type_decl.name; // Simplified for now
    add_interned(r, h, t);
    return t;
}

// Utilities

static void type_mangle_recursive(Arena* arena, TypeInfo* t, char* buf, size_t* pos, size_t cap) {
    if (!t) return;
    switch (t->kind) {
        case TYPE_VOID: *pos += snprintf(buf + *pos, cap - *pos, "void"); break;
        case TYPE_INT: *pos += snprintf(buf + *pos, cap - *pos, "int64_t"); break;
        case TYPE_FLOAT: *pos += snprintf(buf + *pos, cap - *pos, "double"); break;
        case TYPE_BOOL: *pos += snprintf(buf + *pos, cap - *pos, "rae_Bool"); break;
        case TYPE_CHAR: *pos += snprintf(buf + *pos, cap - *pos, "uint32_t"); break;
        case TYPE_STRING: *pos += snprintf(buf + *pos, cap - *pos, "rae_String"); break;
        case TYPE_ANY:
        case TYPE_OPT: *pos += snprintf(buf + *pos, cap - *pos, "RaeAny"); break;
        case TYPE_BUFFER:
            *pos += snprintf(buf + *pos, cap - *pos, "Buffer_");
            type_mangle_recursive(arena, t->as.buffer.base, buf, pos, cap);
            break;
        case TYPE_REF:
            *pos += snprintf(buf + *pos, cap - *pos, t->as.ref.is_mod ? "rae_Mod_" : "rae_View_");
            type_mangle_recursive(arena, t->as.ref.base, buf, pos, cap);
            break;
        case TYPE_STRUCT: {
            *pos += snprintf(buf + *pos, cap - *pos, "rae_%.*s", (int)t->name.len, t->name.data);
            if (t->as.structure.generic_count > 0) {
                *pos += snprintf(buf + *pos, cap - *pos, "_");
                for (size_t i = 0; i < t->as.structure.generic_count; i++) {
                    type_mangle_recursive(arena, t->as.structure.generic_args[i], buf, pos, cap);
                    if (i < t->as.structure.generic_count - 1) *pos += snprintf(buf + *pos, cap - *pos, "_");
                }
            }
            break;
        }
        case TYPE_GENERIC_PARAM:
            *pos += snprintf(buf + *pos, cap - *pos, "rae_%.*s", (int)t->as.generic_param.param_name.len, t->as.generic_param.param_name.data);
            break;
    }
}

Str type_mangle_name(Arena* arena, TypeInfo* t) {
    char buf[1024];
    size_t pos = 0;
    type_mangle_recursive(arena, t, buf, &pos, sizeof(buf));
    
    // Sanitize
    for (size_t i = 0; i < pos; i++) {
        if (!((buf[i] >= 'a' && buf[i] <= 'z') || (buf[i] >= 'A' && buf[i] <= 'Z') || (buf[i] >= '0' && buf[i] <= '9') || buf[i] == '_')) {
            buf[i] = '_';
        }
    }
    
    char* res = arena_alloc(arena, pos + 1);
    memcpy(res, buf, pos + 1);
    return (Str){(uint8_t*)res, pos};
}

bool type_is_same(TypeInfo* a, TypeInfo* b) {
    return a == b; // Interning guarantees this!
}

bool type_is_primitive(TypeInfo* t) {
    return t->kind >= TYPE_VOID && t->kind <= TYPE_CHAR;
}

bool type_is_numeric(TypeInfo* t) {
    return t->kind == TYPE_INT || t->kind == TYPE_FLOAT;
}
