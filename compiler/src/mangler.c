#include "mangler.h"
#include "ast.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    const char* rae_name;
    const char* c_name;
} NativeMap;

static const NativeMap RAYLIB_MAP[] = {
    {"initWindow", "InitWindow"},
    {"windowShouldClose", "WindowShouldClose"},
    {"closeWindow", "CloseWindow"},
    {"beginDrawing", "BeginDrawing"},
    {"endDrawing", "EndDrawing"},
    {"setTargetFPS", "SetTargetFPS"},
    {"getScreenWidth", "GetScreenWidth"},
    {"getScreenHeight", "GetScreenHeight"},
    {"isKeyDown", "IsKeyDown"},
    {"isKeyPressed", "IsKeyPressed"},
    {"clearBackground", "ClearBackground"},
    {"loadTexture", "LoadTexture"},
    {"unloadTexture", "UnloadTexture"},
    {"drawTexture", "DrawTexture"},
    {"drawTextureEx", "DrawTextureEx"},
    {"drawRectangleGradientV", "DrawRectangleGradientV"},
    {"drawRectangleGradientH", "DrawRectangleGradientH"},
    {"drawCircle", "DrawCircle"},
    {"drawCircleGradient", "DrawCircleGradient"},
    {"drawText", "DrawText"},
    {"drawCube", "DrawCube"},
    {"drawSphere", "DrawSphere"},
    {"drawCylinder", "DrawCylinder"},
    {"drawGrid", "DrawGrid"},
    {"beginMode3D", "BeginMode3D"},
    {"endMode3D", "EndMode3D"},
    {NULL, NULL}
};

const char* find_raylib_mapping(Str name) {
    for (int i = 0; RAYLIB_MAP[i].rae_name; i++) {
        if (str_eq_cstr(name, RAYLIB_MAP[i].rae_name)) return RAYLIB_MAP[i].c_name;
    }
    return NULL;
}

const char* map_rae_type_to_c(Str type_name) {
  if (str_eq_cstr(type_name, "Int64") || str_eq_cstr(type_name, "Int")) return "int64_t";
  if (str_eq_cstr(type_name, "Int32")) return "int32_t";
  if (str_eq_cstr(type_name, "UInt64")) return "uint64_t";
  if (str_eq_cstr(type_name, "UInt32")) return "uint32_t";
  if (str_eq_cstr(type_name, "Id")) return "int64_t";
  if (str_eq_cstr(type_name, "Key")) return "rae_String";
  if (str_eq_cstr(type_name, "Float64") || str_eq_cstr(type_name, "Float")) return "double";
  if (str_eq_cstr(type_name, "Float32")) return "float";
  if (str_eq_cstr(type_name, "Bool")) return "rae_Bool";
  if (str_eq_cstr(type_name, "Char32")) return "uint32_t";
  if (str_eq_cstr(type_name, "String")) return "rae_String";
  if (str_eq_cstr(type_name, "Buffer")) return "void";
  if (str_eq_cstr(type_name, "Any")) return "RaeAny";
  if (str_eq_cstr(type_name, "List")) return "rae_List";
  if (str_eq_cstr(type_name, "StringMap")) return "rae_StringMap";
  if (str_eq_cstr(type_name, "IntMap")) return "rae_IntMap";
  if (str_eq_cstr(type_name, "Box")) return "rae_Box";
  return NULL;
}

bool is_primitive_type(Str type_name) {
    return str_eq_cstr(type_name, "Int64") || 
           str_eq_cstr(type_name, "Int") || 
           str_eq_cstr(type_name, "Int32") || 
           str_eq_cstr(type_name, "UInt64") || 
           str_eq_cstr(type_name, "UInt32") || 
           str_eq_cstr(type_name, "Id") || 
           str_eq_cstr(type_name, "Key") || 
           str_eq_cstr(type_name, "Float64") || 
           str_eq_cstr(type_name, "Float") || 
           str_eq_cstr(type_name, "Float32") || 
           str_eq_cstr(type_name, "Bool") || 
           str_eq_cstr(type_name, "Char32") ||
           str_eq_cstr(type_name, "Char") ||
           str_eq_cstr(type_name, "String") ||
           str_eq_cstr(type_name, "Any") ||
           str_eq_cstr(type_name, "int64_t") ||
           str_eq_cstr(type_name, "int32_t") ||
           str_eq_cstr(type_name, "uint64_t") ||
           str_eq_cstr(type_name, "uint32_t") ||
           str_eq_cstr(type_name, "double") ||
           str_eq_cstr(type_name, "float") ||
           str_eq_cstr(type_name, "int8_t") ||
           str_eq_cstr(type_name, "rae_Bool") ||
           str_eq_cstr(type_name, "const_char_p") ||
           str_eq_cstr(type_name, "rae_String") ||
           str_eq_cstr(type_name, "const char*") ||
           str_eq_cstr(type_name, "RaeAny");
}

bool is_raylib_builtin_type(Str type_name) {
    return str_eq_cstr(type_name, "Vector2") || 
           str_eq_cstr(type_name, "Vector3") || 
           str_eq_cstr(type_name, "Color") ||
           str_eq_cstr(type_name, "Texture") ||
           str_eq_cstr(type_name, "Camera3D");
}

static Str get_base_type_name(const AstTypeRef* type) {
    if (!type || !type->parts) return (Str){0};
    return type->parts->text;
}

static void mangle_type_recursive(CompilerContext* ctx, const struct AstIdentifierPart* generic_params, const AstTypeRef* type, char* buf, size_t* pos, size_t cap, bool force_erase) {
    (void)force_erase; // Monomorphisation means we don't erase
    if (!type || !type->parts) {
        *pos += snprintf(buf + *pos, cap - *pos, "int64_t");
        return;
    }
    Str base = type->parts->text;
    
    // Check if it's a generic param
    bool is_generic_param = false;
    if (generic_params) {
        const struct AstIdentifierPart* gp = generic_params;
        while (gp) {
            if (str_eq(gp->text, base)) {
                is_generic_param = true;
                break;
            }
            gp = gp->next;
        }
    }

    const char* mapped = map_rae_type_to_c(base);
    if (mapped) {
        // Map to stable C name
        if (str_eq_cstr(base, "String")) {
            *pos += snprintf(buf + *pos, cap - *pos, "rae_String");
        } else if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int")) {
            *pos += snprintf(buf + *pos, cap - *pos, "int64_t");
        } else if (str_eq_cstr(base, "Int32")) {
            *pos += snprintf(buf + *pos, cap - *pos, "int32_t");
        } else if (str_eq_cstr(base, "UInt64")) {
            *pos += snprintf(buf + *pos, cap - *pos, "uint64_t");
        } else if (str_eq_cstr(base, "UInt32")) {
            *pos += snprintf(buf + *pos, cap - *pos, "uint32_t");
        } else if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float")) {
            *pos += snprintf(buf + *pos, cap - *pos, "double");
        } else if (str_eq_cstr(base, "Float32")) {
            *pos += snprintf(buf + *pos, cap - *pos, "float");
        } else if (str_eq_cstr(base, "Bool")) {
            *pos += snprintf(buf + *pos, cap - *pos, "rae_Bool");
        } else if (str_eq_cstr(base, "Char32")) {
            *pos += snprintf(buf + *pos, cap - *pos, "uint32_t");
        } else if (str_eq_cstr(base, "Any")) {
            *pos += snprintf(buf + *pos, cap - *pos, "RaeAny");
        } else if (str_eq_cstr(base, "Buffer")) {
            Str arg_base = {0};
            if (type->generic_args) arg_base = get_base_type_name(type->generic_args);
            if (arg_base.len == 0 || str_eq_cstr(arg_base, "Any") || str_eq_cstr(arg_base, "RaeAny")) {
                *pos += snprintf(buf + *pos, cap - *pos, "void_p");
                return;
            }
            *pos += snprintf(buf + *pos, cap - *pos, "rae_Buffer");
        } else {
            *pos += snprintf(buf + *pos, cap - *pos, "%s", mapped);
        }
    } else if (str_eq_cstr(base, "int64_t") || str_eq_cstr(base, "double") || str_eq_cstr(base, "int8_t") || str_eq_cstr(base, "int32_t") || str_eq_cstr(base, "const_char_p") || str_eq_cstr(base, "rae_String") || str_eq_cstr(base, "RaeAny") || str_starts_with_cstr(base, "rae_")) {
        *pos += snprintf(buf + *pos, cap - *pos, "%.*s", (int)base.len, base.data);
    } else {
        *pos += snprintf(buf + *pos, cap - *pos, "rae_%.*s", (int)base.len, base.data);
    }
    
    if (type->generic_args) {
        *pos += snprintf(buf + *pos, cap - *pos, "_");
        for (const AstTypeRef* a = type->generic_args; a; a = a->next) {
            mangle_type_recursive(ctx, generic_params, a, buf, pos, cap, false);
            if (a->next) *pos += snprintf(buf + *pos, cap - *pos, "_");
        }
    }
}

#include <ctype.h>

static void sanitize_mangled_name(char* name) {
    if (!name) return;
    for (char* p = name; *p; p++) {
        if (!isalnum(*p) && *p != '_') {
            *p = '_';
        }
    }
}

static void mangle_type_recursive_specialized(CompilerContext* ctx, const struct AstIdentifierPart* generic_params, const AstTypeRef* concrete_args, const AstTypeRef* type, char* buf, size_t* pos, size_t cap) {
    if (!type || !type->parts) {
        *pos += snprintf(buf + *pos, cap - *pos, "int64_t");
        return;
    }
    Str base = type->parts->text;
    
    // Check if it's a generic param that needs substitution
    if (generic_params) {
        const struct AstIdentifierPart* gp = generic_params;
        const AstTypeRef* arg = concrete_args;
        while (gp) {
            if (str_eq(gp->text, base)) {
                if (arg) {
                    // Perform substitution! Mangle the argument instead.
                    mangle_type_recursive_specialized(ctx, NULL, NULL, arg, buf, pos, cap);
                    return;
                } else {
                    // It's a generic parameter but we don't have a concrete argument yet.
                    // This happens during prototype emission for generic functions.
                    *pos += snprintf(buf + *pos, cap - *pos, "rae_%.*s", (int)base.len, base.data);
                    return;
                }
            }
            gp = gp->next; if (arg) arg = arg->next;
        }
    }

    const char* mapped = map_rae_type_to_c(base);
    if (mapped) {
        if (str_eq_cstr(base, "String")) *pos += snprintf(buf + *pos, cap - *pos, "rae_String");
        else if (str_eq_cstr(base, "Int64")) *pos += snprintf(buf + *pos, cap - *pos, "int64_t");
        else if (str_eq_cstr(base, "Int32")) *pos += snprintf(buf + *pos, cap - *pos, "int32_t");
        else if (str_eq_cstr(base, "UInt64")) *pos += snprintf(buf + *pos, cap - *pos, "uint64_t");
        else if (str_eq_cstr(base, "UInt32")) *pos += snprintf(buf + *pos, cap - *pos, "uint32_t");
        else if (str_eq_cstr(base, "Float64")) *pos += snprintf(buf + *pos, cap - *pos, "double");
        else if (str_eq_cstr(base, "Float32")) *pos += snprintf(buf + *pos, cap - *pos, "float");
        else if (str_eq_cstr(base, "Bool")) *pos += snprintf(buf + *pos, cap - *pos, "rae_Bool");
        else if (str_eq_cstr(base, "Char32")) *pos += snprintf(buf + *pos, cap - *pos, "uint32_t");
        else if (str_eq_cstr(base, "Any")) *pos += snprintf(buf + *pos, cap - *pos, "RaeAny");
        else if (str_eq_cstr(base, "Buffer")) {
            Str arg_base = {0};
            if (type->generic_args) arg_base = get_base_type_name(type->generic_args);
            if (arg_base.len == 0 || str_eq_cstr(arg_base, "Any") || str_eq_cstr(arg_base, "RaeAny")) {
                *pos += snprintf(buf + *pos, cap - *pos, "void_p");
                return;
            }
            *pos += snprintf(buf + *pos, cap - *pos, "rae_Buffer");
        }
        else *pos += snprintf(buf + *pos, cap - *pos, "%s", mapped);
    } else if (str_eq_cstr(base, "int64_t") || str_eq_cstr(base, "double") || str_eq_cstr(base, "int8_t") || str_eq_cstr(base, "int32_t") || str_eq_cstr(base, "uint64_t") || str_eq_cstr(base, "uint32_t") || str_eq_cstr(base, "float") || str_eq_cstr(base, "const_char_p") || str_eq_cstr(base, "rae_String") || str_eq_cstr(base, "RaeAny") || str_starts_with_cstr(base, "rae_")) {
        *pos += snprintf(buf + *pos, cap - *pos, "%.*s", (int)base.len, base.data);
    } else {
        *pos += snprintf(buf + *pos, cap - *pos, "rae_%.*s", (int)base.len, base.data);
    }
    
    if (type->generic_args) {
        *pos += snprintf(buf + *pos, cap - *pos, "_");
        for (const AstTypeRef* a = type->generic_args; a; a = a->next) {
            mangle_type_recursive_specialized(ctx, generic_params, concrete_args, a, buf, pos, cap);
            if (a->next) *pos += snprintf(buf + *pos, cap - *pos, "_");
        }
    }
}

const char* rae_mangle_type_specialized(CompilerContext* ctx, const AstIdentifierPart* generic_params, const AstTypeRef* concrete_args, const AstTypeRef* type) {
    char buf[1024];
    size_t pos = 0;
    mangle_type_recursive_specialized(ctx, generic_params, concrete_args, type, buf, &pos, sizeof(buf));
    char* result = arena_alloc(ctx->ast_arena, pos + 1);
    memcpy(result, buf, pos + 1);
    sanitize_mangled_name(result);
    return result;
}

const char* rae_mangle_type_ext(CompilerContext* ctx, const struct AstIdentifierPart* generic_params, const AstTypeRef* type, bool force_erase) {
    char buf[1024];
    size_t pos = 0;
    mangle_type_recursive(ctx, generic_params, type, buf, &pos, sizeof(buf), force_erase);
    
    char* result = arena_alloc(ctx->ast_arena, pos + 1);
    memcpy(result, buf, pos + 1);
    sanitize_mangled_name(result);
    return result;
}

const char* rae_mangle_type(CompilerContext* ctx, const struct AstIdentifierPart* generic_params, const AstTypeRef* type) {
    return rae_mangle_type_ext(ctx, generic_params, type, false);
}

const char* rae_mangle_function(CompilerContext* ctx, const AstFuncDecl* func) {
    if (!func) return "unknown";
    
    if (func->specialization_args) {
        return rae_mangle_specialized_function(ctx, func, func->specialization_args);
    }
    
    if (find_raylib_mapping(func->name)) {
        char* res = arena_alloc(ctx->ast_arena, func->name.len + 9);
        sprintf(res, "rae_ext_%.*s", (int)func->name.len, func->name.data);
        return res;
    }

    if (func->is_extern) {
        Str name = func->name;
        const char* mapped = NULL;
        
        if (str_eq_cstr(name, "sleep") || str_eq_cstr(name, "sleepMs")) mapped = "rae_ext_rae_sleep";
        else if (str_eq_cstr(name, "rae_str") || str_eq_cstr(name, "str")) mapped = "rae_ext_rae_str";
        else if (str_eq_cstr(name, "rae_str_len") || str_eq_cstr(name, "str_len")) mapped = "rae_ext_rae_str_len";
        else if (str_eq_cstr(name, "rae_str_concat") || str_eq_cstr(name, "str_concat")) mapped = "rae_ext_rae_str_concat";
        else if (str_eq_cstr(name, "rae_str_compare") || str_eq_cstr(name, "str_compare")) mapped = "rae_ext_rae_str_compare";
        else if (str_eq_cstr(name, "rae_str_sub") || str_eq_cstr(name, "str_sub")) mapped = "rae_ext_rae_str_sub";
        else if (str_eq_cstr(name, "rae_str_contains") || str_eq_cstr(name, "str_contains")) mapped = "rae_ext_rae_str_contains";
        else if (str_eq_cstr(name, "rae_str_starts_with") || str_eq_cstr(name, "str_starts_with")) mapped = "rae_ext_rae_str_starts_with";
        else if (str_eq_cstr(name, "rae_str_ends_with") || str_eq_cstr(name, "str_ends_with")) mapped = "rae_ext_rae_str_ends_with";
        else if (str_eq_cstr(name, "rae_str_index_of") || str_eq_cstr(name, "str_index_of")) mapped = "rae_ext_rae_str_index_of";
        else if (str_eq_cstr(name, "rae_str_trim") || str_eq_cstr(name, "str_trim")) mapped = "rae_ext_rae_str_trim";
        else if (str_eq_cstr(name, "rae_str_to_f64") || str_eq_cstr(name, "str_to_float")) mapped = "rae_ext_rae_str_to_f64";
        else if (str_eq_cstr(name, "rae_str_to_i64") || str_eq_cstr(name, "str_to_int")) mapped = "rae_ext_rae_str_to_i64";
        else if (str_eq_cstr(name, "getEnv")) mapped = "rae_ext_rae_sys_get_env";
        else if (str_eq_cstr(name, "exit")) mapped = "rae_ext_rae_sys_exit";
        else if (str_eq_cstr(name, "readFile")) mapped = "rae_ext_rae_sys_read_file";
        else if (str_eq_cstr(name, "writeFile")) mapped = "rae_ext_rae_sys_write_file";
        else if (str_eq_cstr(name, "nextTick")) mapped = "rae_ext_nextTick";
        else if (str_eq_cstr(name, "nowMs")) mapped = "rae_ext_nowMs";
        else if (str_eq_cstr(name, "rae_random")) mapped = "rae_ext_rae_random";
        else if (str_eq_cstr(name, "rae_random_int")) mapped = "rae_ext_rae_random_int";
        else if (str_eq_cstr(name, "random")) {
            uint16_t c = 0; for (const AstParam* p = func->params; p; p = p->next) c++;
            if (c == 0) mapped = "rae_ext_rae_random";
            else if (c == 2) mapped = "rae_ext_rae_random_int";
        }
        else if (str_eq_cstr(name, "random_int")) mapped = "rae_ext_rae_random_int";
        else if (str_eq_cstr(name, "rae_int_to_float")) mapped = "rae_ext_rae_int_to_float";
        else if (str_eq_cstr(name, "readLine")) mapped = "rae_ext_rae_io_read_line";
        else if (str_eq_cstr(name, "readChar")) mapped = "rae_ext_rae_io_read_char";
        else if (str_eq_cstr(name, "sin")) mapped = "rae_ext_rae_math_sin";
        else if (str_eq_cstr(name, "cos")) mapped = "rae_ext_rae_math_cos";
        else if (str_eq_cstr(name, "tan")) mapped = "rae_ext_rae_math_tan";
        else if (str_eq_cstr(name, "asin")) mapped = "rae_ext_rae_math_asin";
        else if (str_eq_cstr(name, "acos")) mapped = "rae_ext_rae_math_acos";
        else if (str_eq_cstr(name, "atan")) mapped = "rae_ext_rae_math_atan";
        else if (str_eq_cstr(name, "atan2")) mapped = "rae_ext_rae_math_atan2";
        else if (str_eq_cstr(name, "sqrt")) mapped = "rae_ext_rae_math_sqrt";
        else if (str_eq_cstr(name, "pow")) mapped = "rae_ext_rae_math_pow";
        else if (str_eq_cstr(name, "exp")) mapped = "rae_ext_rae_math_exp";
        else if (str_eq_cstr(name, "math_log")) mapped = "rae_ext_rae_math_log";
        else if (str_eq_cstr(name, "floor")) mapped = "rae_ext_rae_math_floor";
        else if (str_eq_cstr(name, "ceil")) mapped = "rae_ext_rae_math_ceil";
        else if (str_eq_cstr(name, "round")) mapped = "rae_ext_rae_math_round";

        if (mapped) {
            char* res = arena_alloc(ctx->ast_arena, strlen(mapped) + 1);
            strcpy(res, mapped);
            return res;
        }

        if (str_starts_with_cstr(func->name, "rae_ext_")) {
            char* res = arena_alloc(ctx->ast_arena, func->name.len + 1);
            sprintf(res, "%.*s", (int)func->name.len, func->name.data);
            return res;
        }

        char* res = arena_alloc(ctx->ast_arena, func->name.len + 9);
        sprintf(res, "rae_ext_%.*s", (int)func->name.len, func->name.data);
        return res;
    }

    char buf[2048];
    size_t pos = snprintf(buf, sizeof(buf), "rae_%.*s_", (int)func->name.len, func->name.data);
    
    for (const AstParam* p = func->params; p; p = p->next) {
        const char* mangled_param = rae_mangle_type_specialized(ctx, func->generic_params, NULL, p->type);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s_", mangled_param);
    }
    
    char* result = arena_alloc(ctx->ast_arena, pos + 1);
    memcpy(result, buf, pos + 1);
    sanitize_mangled_name(result);
    return result;
}

const char* rae_mangle_specialized_function(CompilerContext* ctx, const AstFuncDecl* func, const AstTypeRef* concrete_args) {
    if (!func) return "unknown";
    if (!concrete_args) return rae_mangle_function(ctx, func);
    
    char buf[2048];
    size_t pos = snprintf(buf, sizeof(buf), "rae_%.*s_", (int)func->name.len, func->name.data);
    
    // Include generic arguments in the name to distinguish specializations
    for (const AstTypeRef* a = concrete_args; a; a = a->next) {
        const char* mangled_arg = rae_mangle_type_specialized(ctx, NULL, NULL, a);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s_", mangled_arg);
    }

    for (const AstParam* p = func->params; p; p = p->next) {
        const char* mangled_param = rae_mangle_type_specialized(ctx, func->generic_params, concrete_args, p->type);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s_", mangled_param);
    }
    
    char* result = arena_alloc(ctx->ast_arena, pos + 1);
    memcpy(result, buf, pos + 1);
    sanitize_mangled_name(result);
    return result;
}
