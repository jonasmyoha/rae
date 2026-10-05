// C-header -> Rae FFI binding generator (general FFI, #498).
//
// A focused, dependency-free parser for the regular WebGPU C header style
// (webgpu.h + wgpu.h), emitting low-level Rae bindings that bind DIRECTLY to
// the C ABI via c_struct types and extern("symbol") functions — no shim.
// Deterministic: the same headers always produce byte-identical output.
//
// It is intentionally not a general C parser. It recognises the specific,
// regular constructs these headers use (see the census in docs/webgpu-bindings.md),
// and, behind options, those of Box3D's headers (docs/physics-two-implementations.md
// §4): functions marked by an export macro (`--api-macro B3_API`) and static
// inline helpers with bodies (`--inline-macro B3_INLINE`), enum members with
// implicit values, `typedef T Alias;`, function-type callbacks
// (`typedef R Name(params);`), comma-separated fields, and structs with nested
// unions or array fields (those fields are skipped; the real C definition is
// used, so a skipped field only means Rae cannot name it). Output longer than a
// file may be is split into numbered parts (`Box3dTypes`, `Box3dTypes2`, ...),
// each importing the type parts before it.
//   typedef enum X { M = v, ... } X;
//   typedef struct XImpl* X;                 (opaque handle)
//   typedef WGPUFlags X;                     (flag set, uint64)
//   static const X X_Member = v;             (enum/flag values)
//   typedef struct X { fields } X;           (struct / descriptor)
//   WGPU_EXPORT RET wgpuName(params);        (function)
//   typedef RET (*XCallback)(params);        (callback fn pointer)
//   #define WGPU_X value                     (constant)
//
// Anything it does not recognise is reported (counted, and echoed to stderr in
// --verbose) rather than silently dropped.
#include "bindgen.h"
#include "bindgen_preprocess.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// Type-kind registry: classify every C type name so pointer/field/param
// mapping is correct (a pointer to a struct is a `view`, a handle is a `Ptr`).
// ---------------------------------------------------------------------------
typedef enum { TK_HANDLE, TK_ENUM, TK_FLAGS, TK_STRUCT, TK_CALLBACK } TypeKind;

typedef struct { char name[96]; TypeKind kind; } TypeEntry;

#define BG_MAX_TYPES 2048
static TypeEntry g_types[BG_MAX_TYPES];
static int g_type_count = 0;

static void reg_type(const char* name, TypeKind kind) {
    if (g_type_count >= BG_MAX_TYPES) return;
    for (int i = 0; i < g_type_count; i++) if (strcmp(g_types[i].name, name) == 0) return;
    snprintf(g_types[g_type_count].name, sizeof(g_types[g_type_count].name), "%s", name);
    g_types[g_type_count].kind = kind;
    g_type_count++;
}

static const char* resolve_alias(const char* name);

static bool type_is(const char* name, TypeKind kind) {
    name = resolve_alias(name);
    for (int i = 0; i < g_type_count; i++)
        if (strcmp(g_types[i].name, name) == 0) return g_types[i].kind == kind;
    return false;
}
static bool type_known(const char* name, TypeKind* out) {
    name = resolve_alias(name);
    for (int i = 0; i < g_type_count; i++)
        if (strcmp(g_types[i].name, name) == 0) { if (out) *out = g_types[i].kind; return true; }
    return false;
}

// Coverage counters, reported at the end.
static int g_n_enum = 0, g_n_flags = 0, g_n_handle = 0, g_n_struct = 0,
           g_n_func = 0, g_n_const = 0, g_n_callback = 0, g_n_skipped = 0;
static bool g_verbose = false;

// Options (bindgen_run). The defaults are the WebGPU headers'.
static const char* g_api_macro = NULL;      // functions are `API RET name(params);`
static const char* g_inline_macros[8];      // `INLINE RET name(params) { body }`
static int g_n_inline_macros = 0;
static const char* g_drop_macros[16];       // blanked before parsing
static int g_n_drop_macros = 0;
static const char* g_define_prefix = "WGPU_";


// `typedef T Alias;` of a known type: Alias resolves to T everywhere.
typedef struct { char alias[96]; char target[96]; } AliasEntry;
#define BG_MAX_ALIASES 256
static AliasEntry g_aliases[BG_MAX_ALIASES];
static int g_alias_count = 0;

static const char* resolve_alias(const char* name) {
    for (int depth = 0; depth < 8; depth++) {
        bool found = false;
        for (int i = 0; i < g_alias_count; i++) {
            if (strcmp(g_aliases[i].alias, name) == 0) { name = g_aliases[i].target; found = true; break; }
        }
        if (!found) break;
    }
    return name;
}

static void skipped(const char* what) {
    g_n_skipped++;
    if (g_verbose) fprintf(stderr, "[bindgen] skipped: %s\n", what);
}

// ---------------------------------------------------------------------------
// Source loading + cleaning
// ---------------------------------------------------------------------------
static char* read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[bindgen] cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char* buf = malloc((size_t)n + 1);
    size_t rd = fread(buf, 1, (size_t)n, f); fclose(f);
    buf[rd] = '\0'; if (out_len) *out_len = rd;
    return buf;
}

// Replace /* */ and // comments with spaces (keeps offsets/newlines sane).
static void strip_comments(char* s) {
    for (size_t i = 0; s[i]; ) {
        if (s[i] == '/' && s[i+1] == '*') {
            s[i] = s[i+1] = ' '; i += 2;
            while (s[i] && !(s[i] == '*' && s[i+1] == '/')) { if (s[i] != '\n') s[i] = ' '; i++; }
            if (s[i]) { s[i] = ' '; s[i+1] = ' '; i += 2; }
        } else if (s[i] == '/' && s[i+1] == '/') {
            while (s[i] && s[i] != '\n') s[i++] = ' ';
        } else i++;
    }
}

// Blank out attribute/nullability macros so declarations read cleanly. Keeps
// WGPU_EXPORT (the function marker). Removes any WGPU*ATTRIBUTE and the
// nullability hints.
static bool is_ident_ch(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void remove_macros(char* s) {
    static const char* drop[] = {
        "WGPU_NULLABLE", "WGPU_NONNULL", "WGPU_OBJECT_ATTRIBUTE",
        "WGPU_ENUM_ATTRIBUTE", "WGPU_STRUCTURE_ATTRIBUTE",
        "WGPU_FUNCTION_ATTRIBUTE", "WGPU_EXPORT", NULL
    };
    for (size_t i = 0; s[i]; ) {
        if (!is_ident_ch(s[i]) || (i > 0 && is_ident_ch(s[i-1]))) { i++; continue; }
        size_t j = i; while (s[j] && is_ident_ch(s[j])) j++;
        size_t len = j - i;
        bool matched = false;
        for (int k = 0; drop[k]; k++) {
            if (strlen(drop[k]) == len && strncmp(s + i, drop[k], len) == 0) { matched = true; break; }
        }
        for (int k = 0; k < g_n_drop_macros && !matched; k++) {
            if (strlen(g_drop_macros[k]) == len && strncmp(s + i, g_drop_macros[k], len) == 0) matched = true;
        }
        // Also drop any leftover WGPU_..._ATTRIBUTE spelled differently.
        if (!matched && len > 10 && strncmp(s + i, "WGPU", 4) == 0) {
            if (len >= 9 && strncmp(s + j - 9, "ATTRIBUTE", 9) == 0) matched = true;
        }
        if (matched) { for (size_t k = i; k < j; k++) s[k] = ' '; }
        i = j;
    }
}

// ---------------------------------------------------------------------------
// Small scanner helpers over a cursor
// ---------------------------------------------------------------------------
static void skip_ws(const char** p) { while (**p && isspace((unsigned char)**p)) (*p)++; }

static bool starts_kw(const char* p, const char* kw) {
    size_t n = strlen(kw);
    if (strncmp(p, kw, n) != 0) return false;
    return !is_ident_ch(p[n]); // whole-word
}

// Copy the next identifier at *p into out; advance. Returns false if none.
static bool take_ident(const char** p, char* out, size_t cap) {
    skip_ws(p);
    if (!(**p) || !(isalpha((unsigned char)**p) || **p == '_')) return false;
    size_t n = 0; while (is_ident_ch(**p)) { if (n + 1 < cap) out[n++] = **p; (*p)++; }
    out[n] = '\0'; return true;
}

// ---------------------------------------------------------------------------
// C integer literal -> Rae. Strips parens and U/L suffixes. Returns false if it
// cannot be represented as a plain Rae Int64/UInt64 literal.
// ---------------------------------------------------------------------------
static bool parse_int_literal(const char* raw, char* out, size_t cap, bool* out_wide) {
    // Trim outer whitespace.
    char work[160]; size_t wn = 0;
    for (const char* q = raw; *q && wn + 1 < sizeof(work); q++) work[wn++] = *q;
    work[wn] = '\0';
    char* a = work; while (*a && isspace((unsigned char)*a)) a++;
    char* b = a + strlen(a); while (b > a && isspace((unsigned char)b[-1])) *--b = '\0';
    // Peel matched outer parens repeatedly: (X) -> X.
    while (*a == '(' && b > a && b[-1] == ')') { a++; *--b = '\0'; while (*a && isspace((unsigned char)*a)) a++; while (b > a && isspace((unsigned char)b[-1])) *--b = '\0'; }
    unsigned long long val;
    // Common stdint sentinel macros used by the WebGPU headers.
    if (!strcmp(a, "UINT32_MAX")) val = 4294967295ULL;
    else if (!strcmp(a, "UINT64_MAX") || !strcmp(a, "SIZE_MAX")) val = 18446744073709551615ULL;
    else if (!strcmp(a, "INT32_MAX")) val = 2147483647ULL;
    else if (!strcmp(a, "INT64_MAX")) val = 9223372036854775807ULL;
    else if (!strncmp(a, "UINT32_C", 8) || !strncmp(a, "UINT64_C", 8) ||
             !strncmp(a, "INT32_C", 7) || !strncmp(a, "INT64_C", 7)) {
        const char* lp = strchr(a, '('); const char* rp = lp ? strchr(lp, ')') : NULL;
        if (!lp || !rp) return false;
        char inner[64]; size_t n = 0; for (const char* q = lp + 1; q < rp && n + 1 < sizeof(inner); q++) if (!isspace((unsigned char)*q)) inner[n++] = *q;
        inner[n] = '\0';
        char* end = NULL; val = strtoull(inner, &end, 0); if (!end || *end != '\0') return false;
    } else {
        // Plain literal: strip integer suffixes, then dec/hex.
        size_t n = strlen(a);
        while (n > 0 && (a[n-1] == 'u' || a[n-1] == 'U' || a[n-1] == 'l' || a[n-1] == 'L')) a[--n] = '\0';
        if (n == 0) return false;
        char* end = NULL; val = strtoull(a, &end, 0); if (!end || *end != '\0') return false;
    }
    if (out_wide) *out_wide = (val > (unsigned long long)INT64_MAX);
    snprintf(out, cap, "%llu", val);
    return true;
}

// ---------------------------------------------------------------------------
// C type -> Rae type. `ctx`: 0 field, 1 param, 2 return.
// ---------------------------------------------------------------------------
static const char* map_scalar(const char* b) {
    if (!strcmp(b, "uint8_t")) return "UInt8";
    if (!strcmp(b, "uint16_t")) return "UInt16";
    if (!strcmp(b, "uint32_t")) return "UInt32";
    if (!strcmp(b, "uint64_t")) return "UInt64";
    if (!strcmp(b, "int8_t")) return "Int8";
    if (!strcmp(b, "int16_t")) return "Int16";
    if (!strcmp(b, "int32_t")) return "Int32";
    if (!strcmp(b, "int64_t")) return "Int64";
    if (!strcmp(b, "size_t")) return "UInt64";
    if (!strcmp(b, "float")) return "Float32";
    if (!strcmp(b, "double")) return "Float64";
    if (!strcmp(b, "WGPUBool")) return "UInt32";
    if (!strcmp(b, "WGPUFlags")) return "UInt64";
    if (!strcmp(b, "WGPUSubmissionIndex")) return "UInt64";
    if (!strcmp(b, "int") || !strcmp(b, "signed int")) return "Int32";
    if (!strcmp(b, "unsigned")) return "UInt32";
    if (!strcmp(b, "bool") || !strcmp(b, "_Bool")) return "Bool";
    if (!strcmp(b, "char")) return "Int8";
    return NULL;
}

// out must be >=128. Returns false if the type cannot be represented.
static bool map_type(const char* base, int ptr, int ctx, char* out, size_t cap) {
    // Pointers.
    if (ptr > 0) {
        // A pointer-to-pointer, or any pointer in a struct FIELD, or void*,
        // stays a raw Ptr — the caller supplies the address / list.data.
        if (ptr >= 2 || !strcmp(base, "void")) { snprintf(out, cap, "Ptr"); return true; }
        TypeKind k;
        if (type_known(base, &k) && k == TK_STRUCT && ctx == 1) {
            // A single struct pointer as a PARAM is a borrow: const -> view,
            // non-const -> mod. (Handled by caller passing is_const via base?)
            // We map to view here; non-const detection is done before calling.
            snprintf(out, cap, "view %s", resolve_alias(base)); return true;
        }
        // struct pointer in a field, handle*, enum*, primitive*, string ptr: Ptr.
        snprintf(out, cap, "Ptr"); return true;
    }
    // Non-pointer.
    if (!strcmp(base, "void")) { out[0] = '\0'; return true; } // return-only
    base = resolve_alias(base);
    const char* sc = map_scalar(base);
    if (sc) { snprintf(out, cap, "%s", sc); return true; }
    TypeKind k;
    if (type_known(base, &k)) {
        switch (k) {
            case TK_ENUM: snprintf(out, cap, "Int32"); return true;
            case TK_FLAGS: snprintf(out, cap, "UInt64"); return true;
            case TK_HANDLE: snprintf(out, cap, "Ptr"); return true;
            case TK_CALLBACK: snprintf(out, cap, "Ptr"); return true;
            case TK_STRUCT:
                // by-value struct: field -> struct type; param -> copy struct.
                if (ctx == 1) snprintf(out, cap, "copy %s", base);
                else snprintf(out, cap, "%s", base);
                return true;
        }
    }
    return false; // unknown type
}

// ---------------------------------------------------------------------------
// Declaration tokeniser: split "TYPE ... * name" into base type, ptr depth,
// const-ness, and the trailing name (name may be empty).
// ---------------------------------------------------------------------------
typedef struct { char base[96]; char name[96]; int ptr; int is_const; bool ok; } Decl;

static Decl parse_decl(const char* text) {
    Decl d; d.base[0] = d.name[0] = '\0'; d.ptr = 0; d.is_const = 0; d.ok = false;
    char toks[24][96]; int nt = 0;
    const char* p = text;
    while (*p) {
        skip_ws(&p);
        if (!*p) break;
        if (*p == '*') { d.ptr++; p++; continue; }
        if (isalpha((unsigned char)*p) || *p == '_') {
            char id[96]; size_t n = 0;
            while (is_ident_ch(*p)) { if (n + 1 < sizeof(id)) id[n++] = *p; p++; }
            id[n] = '\0';
            if (nt < 24) snprintf(toks[nt++], 96, "%s", id);
        } else p++; // skip other punctuation (e.g. [], not expected here)
    }
    if (nt == 0) return d;
    // Trailing token is the name IF there is more than one token; a lone token
    // is the type (unnamed param).
    int base_end = nt;
    if (nt >= 2) { snprintf(d.name, sizeof(d.name), "%s", toks[nt-1]); base_end = nt - 1; }
    // Assemble base from remaining tokens, dropping const/struct/enum keywords.
    char base[96]; base[0] = '\0';
    for (int i = 0; i < base_end; i++) {
        if (!strcmp(toks[i], "const")) { d.is_const = 1; continue; }
        if (!strcmp(toks[i], "struct") || !strcmp(toks[i], "enum") || !strcmp(toks[i], "union")) continue;
        if (base[0]) {
            // multi-word primitive (unsigned char / unsigned int / long long)
            char joined[96]; snprintf(joined, sizeof(joined), "%s %s", base, toks[i]);
            snprintf(base, sizeof(base), "%s", joined);
        } else snprintf(base, sizeof(base), "%s", toks[i]);
    }
    // Normalise common multi-word C types.
    if (!strcmp(base, "unsigned char")) snprintf(base, sizeof(base), "uint8_t");
    else if (!strcmp(base, "unsigned int")) snprintf(base, sizeof(base), "uint32_t");
    else if (!strcmp(base, "unsigned short")) snprintf(base, sizeof(base), "uint16_t");
    else if (!strcmp(base, "unsigned long")) snprintf(base, sizeof(base), "uint64_t");
    else if (!strcmp(base, "long long") || !strcmp(base, "long")) snprintf(base, sizeof(base), "int64_t");
    snprintf(d.base, sizeof(d.base), "%s", base);
    d.ok = (d.base[0] != '\0');
    return d;
}

// ---------------------------------------------------------------------------
// Rae keyword-safe parameter names.
// ---------------------------------------------------------------------------
static const char* safe_name(const char* name, char* buf, size_t cap) {
    static const char* kw[] = {"type","ret","func","let","var","const","if","else",
        "loop","match","view","mod","own","copy","open","import","export","enum",
        "extern","pub","priv","spawn","is","not","and","or","none","true","false", NULL};
    const char* use = (name && name[0]) ? name : "arg";
    for (int i = 0; kw[i]; i++) if (!strcmp(use, kw[i])) { snprintf(buf, cap, "%s_", use); return buf; }
    snprintf(buf, cap, "%s", use);
    return buf;
}

// The '}' matching the '{' at `open` (nested braces counted), or NULL.
static const char* match_brace(const char* open) {
    int depth = 0;
    for (const char* p = open; *p; p++) {
        if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) return p;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Pass 1: classify all type names.
// ---------------------------------------------------------------------------
static void classify(const char* s) {
    const char* p = s;
    while (*p) {
        if (starts_kw(p, "typedef")) {
            const char* q = p + 7; skip_ws(&q);
            if (starts_kw(q, "enum")) {
                q += 4;
                // find closing '}' then the name before ';'
                const char* brace = strchr(q, '{');
                const char* semi = strchr(q, ';');
                if (brace && semi && brace < semi) {
                    const char* close = strchr(brace, '}');
                    if (close) { const char* r = close + 1; char nm[96];
                        if (take_ident(&r, nm, sizeof(nm))) reg_type(nm, TK_ENUM); }
                }
            } else if (starts_kw(q, "struct")) {
                const char* r = q + 6;
                const char* semi = strchr(r, ';');
                const char* brace = strchr(r, '{');
                if (brace && (!semi || brace < semi)) {
                    const char* close = match_brace(brace);
                    if (close) { const char* t = close + 1; char nm[96];
                        if (take_ident(&t, nm, sizeof(nm))) reg_type(nm, TK_STRUCT); }
                } else if (semi) {
                    // typedef struct XImpl* X;  -> handle X
                    // last identifier before ';'
                    char last[96]; last[0] = '\0'; const char* t = r;
                    while (t < semi) { char id[96]; if (take_ident(&t, id, sizeof(id))) snprintf(last, sizeof(last), "%s", id); else t++; }
                    if (last[0]) reg_type(last, TK_HANDLE);
                }
            } else if (starts_kw(q, "WGPUFlags")) {
                const char* r = q + 9; char nm[96];
                if (take_ident(&r, nm, sizeof(nm))) reg_type(nm, TK_FLAGS);
            } else {
                // possible callback: typedef RET (*Name)(...);
                const char* star = strstr(q, "(*");
                const char* semi = strchr(q, ';');
                const char* paren = strchr(q, '(');
                if (star && semi && star < semi) {
                    const char* r = star + 2; char nm[96];
                    if (take_ident(&r, nm, sizeof(nm))) reg_type(nm, TK_CALLBACK);
                } else if (paren && semi && paren < semi) {
                    // a function type: typedef RET Name(...); (used as Name*)
                    const char* np = paren; while (np > q && isspace((unsigned char)np[-1])) np--;
                    const char* ns = np; while (ns > q && is_ident_ch(ns[-1])) ns--;
                    char nm[96]; size_t n = (size_t)(np - ns);
                    if (n > 0 && n < sizeof(nm)) { memcpy(nm, ns, n); nm[n] = '\0'; reg_type(nm, TK_CALLBACK); }
                } else if (semi) {
                    // an alias: typedef Target Alias;  (registered after pass 1
                    // so the target's kind is known; see register_aliases)
                }
            }
        }
        // advance one token/char
        p++;
    }
}

// ---------------------------------------------------------------------------
// Emit helpers
// ---------------------------------------------------------------------------
// `typedef Target Alias;` where Target is a known type (after classify).
static void register_aliases(const char* s) {
    for (const char* p = s; *p; p++) {
        if (!starts_kw(p, "typedef") || (p > s && is_ident_ch(p[-1]))) continue;
        const char* q = p + 7;
        const char* semi = strchr(q, ';');
        if (!semi) continue;
        char first[96], second[96], extra[96];
        const char* t = q;
        if (!take_ident(&t, first, sizeof(first)) || !take_ident(&t, second, sizeof(second))) continue;
        skip_ws(&t);
        if (t != semi || take_ident(&t, extra, sizeof(extra))) continue;
        if (!type_known(first, NULL) || type_known(second, NULL)) continue;
        if (g_alias_count < BG_MAX_ALIASES) {
            snprintf(g_aliases[g_alias_count].alias, sizeof(g_aliases[0].alias), "%s", second);
            snprintf(g_aliases[g_alias_count].target, sizeof(g_aliases[0].target), "%s", first);
            g_alias_count++;
        }
    }
}

static void emit_enum(FILE* out, const char* body, const char* tname) {
    // body is between '{' and '}'. Members: NAME = VALUE ,
    fprintf(out, "# enum %s\n", tname);
    const char* p = body;
    // A member without `= value` is the previous value + 1 (0 first), as in C;
    // after a member whose value is not a literal the count is unknown.
    unsigned long long next = 0; bool next_known = true;
    while (*p) {
        char nm[96];
        if (!take_ident(&p, nm, sizeof(nm))) { if (*p) p++; continue; }
        skip_ws(&p);
        if (*p != '=') {
            if (next_known) { fprintf(out, "const %s: Int32 = %llu\n", nm, next); next++; }
            else { fprintf(out, "# skipped const %s (implicit value after a non-literal)\n", nm); skipped(nm); }
            while (*p && *p != ',') p++; if (*p) p++; continue;
        }
        p++; // '='
        skip_ws(&p);
        const char* vstart = p;
        while (*p && *p != ',' && *p != '}') p++;
        char raw[128]; size_t len = (size_t)(p - vstart); if (len >= sizeof(raw)) len = sizeof(raw)-1;
        memcpy(raw, vstart, len); raw[len] = '\0';
        char val[64]; bool wide = false;
        if (parse_int_literal(raw, val, sizeof(val), &wide)) {
            fprintf(out, "const %s: Int32 = %s\n", nm, val);
            next = strtoull(val, NULL, 10) + 1; next_known = true;
        } else {
            fprintf(out, "# skipped const %s (non-literal value)\n", nm); skipped(nm);
            next_known = false;
        }
        if (*p == ',') p++;
    }
    fprintf(out, "\n");
}

// One field declarator "TYPE [*]name" of struct `tname`.
static int emit_field(FILE* out, const char* field) {
    Decl d = parse_decl(field);
    if (!d.ok || d.name[0] == '\0') { fprintf(out, "  # skipped field: %s\n", field); skipped("struct field"); return 0; }
    char rae[128];
    if (!map_type(d.base, d.ptr, 0, rae, sizeof(rae)) || rae[0] == '\0') {
        fprintf(out, "  # skipped field %s (unmapped type %s)\n", d.name, d.base); skipped(d.name); return 0;
    }
    char nm[96];
    fprintf(out, "  %s: %s\n", safe_name(d.name, nm, sizeof(nm)), rae);
    return 1;
}

static void emit_struct(FILE* out, const char* body, const char* tname) {
    fprintf(out, "type %s: c_struct {\n", tname);
    const char* p = body;
    int fields = 0;
    while (*p) {
        skip_ws(&p);
        if (!*p) break;
        // One member, up to the ';' outside any nested braces
        const char* start = p; int depth = 0, parens = 0;
        bool nested = false, array = false, comma = false, function = false;
        while (*p && !(depth == 0 && *p == ';')) {
            if (*p == '{') { depth++; nested = true; }
            else if (*p == '}') depth--;
            else if (*p == '(') { parens++; if (depth == 0) function = true; }
            else if (*p == ')') parens--;
            else if (depth == 0 && parens == 0 && *p == '[') array = true;
            else if (depth == 0 && parens == 0 && *p == ',') comma = true;
            p++;
        }
        if (!*p) break;
        char field[512]; size_t len = (size_t)(p - start); if (len >= sizeof(field)) len = sizeof(field)-1;
        memcpy(field, start, len); field[len] = '\0';
        p++;
        bool blank = true; for (size_t i = 0; field[i]; i++) if (!isspace((unsigned char)field[i])) { blank = false; break; }
        if (blank) continue;
        if (nested) { fprintf(out, "  # skipped a nested union/struct member\n"); skipped("nested member"); continue; }
        if (function) {
            // A function-pointer field `RET (*Name)(params)` holds a Ptr
            const char* t = strchr(field, '(');
            if (t) { t++; skip_ws(&t); t = (*t == '*') ? t + 1 : NULL; }
            char nm[96], safe[96];
            if (t && take_ident(&t, nm, sizeof(nm))) { fprintf(out, "  %s: Ptr\n", safe_name(nm, safe, sizeof(safe))); fields++; }
            else { fprintf(out, "  # skipped a function member\n"); skipped("function member"); }
            continue;
        }
        if (array) {
            // Name the field: the identifier before the first '['
            const char* lb = strchr(field, '['); const char* ne = lb;
            while (ne > field && isspace((unsigned char)ne[-1])) ne--;
            const char* ns = ne; while (ns > field && is_ident_ch(ns[-1])) ns--;
            fprintf(out, "  # skipped array field %.*s\n", (int)(ne - ns), ns); skipped("array field");
            continue;
        }
        if (!comma) { fields += emit_field(out, field); continue; }
        // `float x, y, z;`: every declarator shares the first one's base type
        char* save = NULL; char* part = strtok_r(field, ",", &save);
        Decl first = parse_decl(part);
        fields += emit_field(out, part);
        while ((part = strtok_r(NULL, ",", &save)) != NULL) {
            char one[512];
            snprintf(one, sizeof(one), "%s%s %s", first.is_const ? "const " : "", first.base, part);
            fields += emit_field(out, one);
        }
    }
    if (fields == 0) fprintf(out, "  # (opaque / no representable fields)\n");
    fprintf(out, "}\n\n");
}

// Parse & emit one function: text is between WGPU_EXPORT and ';'.
static void emit_function(FILE* out, const char* text) {
    // find '(' and matching ')'
    const char* lp = strchr(text, '(');
    if (!lp) { skipped("function (no paren)"); return; }
    const char* rp = strrchr(text, ')');
    if (!rp || rp < lp) { skipped("function (no close paren)"); return; }
    // function name = identifier immediately before '('
    const char* np = lp; while (np > text && is_ident_ch(np[-1])) np--;
    char fname[128]; size_t fl = (size_t)(lp - np); if (fl >= sizeof(fname)) fl = sizeof(fname)-1;
    memcpy(fname, np, fl); fname[fl] = '\0';
    if (fname[0] == '\0') {
        if (g_verbose) fprintf(stderr, "[bindgen] function with no name: %.80s\n", text);
        skipped("function (no name)"); return;
    }
    // return type = text before the name
    char rettext[256]; size_t rl = (size_t)(np - text); if (rl >= sizeof(rettext)) rl = sizeof(rettext)-1;
    memcpy(rettext, text, rl); rettext[rl] = '\0';
    Decl rd = parse_decl(rettext);
    // params
    char params[4096]; size_t pl = (size_t)(rp - lp - 1); if (pl >= sizeof(params)) pl = sizeof(params)-1;
    memcpy(params, lp + 1, pl); params[pl] = '\0';

    // Build the Rae signature into a temp buffer first, so a single unmapped
    // type makes us skip the WHOLE function cleanly (with a note).
    char sig[8192]; int sp = 0;
    sp += snprintf(sig + sp, sizeof(sig) - sp, "func %s(", fname);

    bool first = true, bad = false; char badtype[96] = "";
    // split params by top-level comma
    Decl decls[32]; int nd = 0;
    const char* q = params;
    while (*q && nd < 32) {
        while (*q && isspace((unsigned char)*q)) q++;
        if (!*q) break;
        const char* start = q; int depth = 0;
        while (*q && !(depth == 0 && *q == ',')) { if (*q=='(') depth++; else if (*q==')') depth--; q++; }
        char one[512]; size_t n = (size_t)(q - start); if (n >= sizeof(one)) n = sizeof(one)-1;
        memcpy(one, start, n); one[n] = '\0';
        if (*q == ',') q++;
        Decl d = parse_decl(one);
        if (!d.ok) continue;
        if (!strcmp(d.base, "void") && d.ptr == 0) continue; // (void)
        decls[nd++] = d;
    }
    for (int di = 0; di < nd && !bad; di++) {
        Decl d = decls[di];
        // A struct pointer followed by an integer count is an array: Ptr (pass
        // a List's .data), not a single borrowed struct.
        bool array_param = false;
        if (d.ptr == 1 && di + 1 < nd && decls[di + 1].ptr == 0 &&
            (strstr(decls[di + 1].name, "count") || strstr(decls[di + 1].name, "Count"))) {
            const char* sc = map_scalar(decls[di + 1].base);
            array_param = sc && (sc[0] == 'I' || sc[0] == 'U');
        }
        char rae[128];
        // param context; const struct-ptr -> view, non-const struct-ptr -> mod
        if (array_param) {
            snprintf(rae, sizeof(rae), "Ptr");
        } else if (d.ptr == 1 && type_is(d.base, TK_STRUCT)) {
            snprintf(rae, sizeof(rae), "%s %s", d.is_const ? "view" : "mod", resolve_alias(d.base));
        } else if (!map_type(d.base, d.ptr, 1, rae, sizeof(rae)) || rae[0] == '\0') {
            bad = true; snprintf(badtype, sizeof(badtype), "%s", d.base); break;
        }
        char nm[96];
        sp += snprintf(sig + sp, sizeof(sig) - sp, "%s%s: %s", first ? "" : ", ",
                       safe_name(d.name, nm, sizeof(nm)), rae);
        first = false;
    }
    if (bad) {
        fprintf(out, "# skipped func %s (unmapped param type %s)\n", fname, badtype);
        skipped(fname); return;
    }
    // #893: every generated declaration is a raw C-ABI call, so it carries the
    // post-parameter `unsafe` modifier BEFORE `extern` (the #868 approved order:
    // `func f(...) unsafe extern("sym") ret T`). This marks the ABI declaration
    // only; callers opt into `unsafe { ... }` in their wrapper/consumer modules.
    sp += snprintf(sig + sp, sizeof(sig) - sp, ") unsafe extern(\"%s\")", fname);
    // return
    char rret[128];
    if (rd.ok && !(rd.base[0] == 'v' && !strcmp(rd.base, "void") && rd.ptr == 0)) {
        if (rd.ptr == 1 && type_is(rd.base, TK_STRUCT)) {
            snprintf(rret, sizeof(rret), "Ptr"); // struct pointer return -> Ptr
            sp += snprintf(sig + sp, sizeof(sig) - sp, " ret Ptr");
        } else if (map_type(rd.base, rd.ptr, 2, rret, sizeof(rret)) && rret[0] != '\0') {
            // a `copy S` return makes no sense; a by-value struct return is rare
            const char* rr = rret; char clean[128];
            if (!strncmp(rret, "copy ", 5)) { snprintf(clean, sizeof(clean), "%s", rret + 5); rr = clean; }
            sp += snprintf(sig + sp, sizeof(sig) - sp, " ret %s", rr);
        } else if (rd.ptr == 0 && !strncmp(rd.base, "WGPU", 4)) {
            // Unmapped non-void return that names a WGPU type is a HANDLE — every
            // enum/flag/struct is registered, so the only way a `WGPU*` return
            // reaches here is a handle whose name the classifier missed (a few
            // GetBindGroupLayout / BeginComputePass entry points hit a parse
            // glitch in the full header). A handle is an opaque Ptr. Emit it
            // rather than dropping the function.
            sp += snprintf(sig + sp, sizeof(sig) - sp, " ret Ptr");
        } else {
            fprintf(out, "# skipped func %s (unmapped return type %s)\n", fname, rd.base);
            skipped(fname); return;
        }
    }
    fprintf(out, "%s\n", sig);
    g_n_func++;
}

// ---------------------------------------------------------------------------
// Output parts. Every emitted item (an enum, a struct, a function, a const) is
// written whole into its kind's current file; when it would push the file past
// the 1000-line cap, the kind continues in a new numbered part
// (`<Base>2.rae`, ...). A types part imports the type parts before it; a
// functions part imports every types part.
// ---------------------------------------------------------------------------
#define BG_MAX_FILE_LINES 990
enum { PART_ENUMS, PART_TYPES, PART_FUNCS };

typedef struct {
    char base[256];
    int part;      // parts opened so far
    FILE* f;
    int lines;     // lines in the open part
    int header_lines;
} PartOut;

static PartOut g_parts[3];
static const char* g_out_dir;
static const char* g_import_prefix;
static const char* g_cheader;
static const char* g_module_comment;
static char g_pascal[256];

static int count_lines(const char* t) { int n = 0; for (; *t; t++) if (*t == '\n') n++; return n; }

static void emit_file_header(FILE* out, const char* module_comment) {
    /* #918: the bindings mirror the C API verbatim, one declaration per line;
     * `rae format` would expand every 4+-parameter extern to a vertical list
     * and push the file over the 1000-line cap. They are a foreign snippet in
     * the formatter's sense, so the whole file is a raefmt-off region. */
    fprintf(out, "# raefmt: off\n");
    fprintf(out, "# GENERATED by `rae bindgen` — do not edit by hand.\n");
    fprintf(out, "# Regenerate with the command documented in docs/webgpu-bindings.md.\n");
    if (module_comment) fprintf(out, "# %s\n", module_comment);
    fprintf(out, "#\n# Low-level Rae bindings to the C ABI (general FFI, #497/#498). Handles are\n");
    fprintf(out, "# opaque Ptr; enums are Int32 consts; flag sets are UInt64 consts; structs\n");
    fprintf(out, "# are c_struct mirrors of the real C types; functions bind via\n");
    fprintf(out, "# `unsafe extern(\"symbol\")` straight to the library (raw C ABI, #868).\n");
    fprintf(out, "# Build ergonomic wrappers — which take the `unsafe { ... }` obligation —\n");
    fprintf(out, "# in a separate module, not here.\n\n");
}

static void part_name(char* out, size_t cap, int kind, int part) {
    const char* suffix = kind == PART_ENUMS ? "Enums" : kind == PART_TYPES ? "Types" : "";
    if (part == 1) snprintf(out, cap, "%s%s", g_pascal, suffix);
    else snprintf(out, cap, "%s%s%d", g_pascal, suffix, part);
}

static bool part_open(int kind) {
    PartOut* o = &g_parts[kind];
    o->part++;
    char name[300], path[1400];
    part_name(name, sizeof(name), kind, o->part);
    snprintf(path, sizeof(path), "%s/%s.rae", g_out_dir, name);
    o->f = fopen(path, "w");
    if (!o->f) { fprintf(stderr, "[bindgen] cannot write %s\n", path); return false; }
    char* buf = NULL; size_t len = 0;
    FILE* m = open_memstream(&buf, &len);
    emit_file_header(m, g_module_comment);
    // Types and functions reference the real C library structs -> need the header.
    if (kind != PART_ENUMS && g_cheader) fprintf(m, "cheader \"%s\"\n\n", g_cheader);
    // Functions use the c_struct types by name (view WGPUXDescriptor); a later
    // types part uses the earlier parts' types.
    int imports = kind == PART_FUNCS ? g_parts[PART_TYPES].part : kind == PART_TYPES ? o->part - 1 : 0;
    for (int k = 1; k <= imports; k++) {
        char tname[300]; part_name(tname, sizeof(tname), PART_TYPES, k);
        fprintf(m, "import %s/%s\n", g_import_prefix, tname);
    }
    if (imports > 0) fprintf(m, "\n");
    fclose(m);
    fputs(buf, o->f);
    o->lines = o->header_lines = count_lines(buf);
    free(buf);
    return true;
}

static void part_write(int kind, const char* text) {
    PartOut* o = &g_parts[kind];
    int n = count_lines(text);
    if (o->f && o->lines + n > BG_MAX_FILE_LINES && o->lines > o->header_lines) { fclose(o->f); o->f = NULL; }
    if (!o->f && !part_open(kind)) return;
    fputs(text, o->f);
    o->lines += n;
}

// Items are emitted into a memory stream, then written whole to their part.
static char* g_item_buf = NULL;
static size_t g_item_len = 0;
static FILE* item_begin(void) { return open_memstream(&g_item_buf, &g_item_len); }
static void item_end(FILE* m, int kind) {
    fclose(m);
    part_write(kind, g_item_buf);
    free(g_item_buf); g_item_buf = NULL; g_item_len = 0;
}

static bool token_at(const char* p, const char* s, const char* word) {
    return word && starts_kw(p, word) && (p == s || !is_ident_ch(p[-1]));
}

// The functions marked by --api-macro (declarations) and --inline-macro
// (static inline definitions, called through the same extern: the generated
// C includes the header, so the inline body is what runs).
static void emit_marked_functions(const char* s) {
    for (const char* p = s; *p; ) {
        // Preprocessor lines (with continuations) are not declarations
        if ((p == s || p[-1] == '\n') && *p == '#') {
            while (*p && !(*p == '\n' && p[-1] != '\\')) p++;
            continue;
        }
        const char* marker = NULL;
        bool is_inline = false;
        if (token_at(p, s, g_api_macro)) marker = g_api_macro;
        for (int k = 0; k < g_n_inline_macros && !marker; k++) {
            if (token_at(p, s, g_inline_macros[k])) { marker = g_inline_macros[k]; is_inline = true; }
        }
        if (!marker) { p++; continue; }
        const char* start = p + strlen(marker);
        const char* lp = strchr(start, '(');
        const char* stop = strpbrk(start, ";{");
        if (!lp || (stop && stop < lp)) { p = start; continue; }
        const char* rp = lp; int depth = 0;
        do { if (*rp == '(') depth++; else if (*rp == ')') depth--; rp++; } while (*rp && depth > 0);
        const char* aq = rp; while (*aq && isspace((unsigned char)*aq)) aq++;
        if ((!is_inline && *aq == ';') || (is_inline && *aq == '{')) {
            char text[4096]; size_t n = (size_t)(rp - start); if (n >= sizeof(text)) n = sizeof(text)-1;
            memcpy(text, start, n); text[n] = '\0';
            FILE* m = item_begin(); emit_function(m, text); item_end(m, PART_FUNCS);
            if (is_inline) { const char* close = match_brace(aq); p = close ? close + 1 : aq + 1; }
            else p = aq + 1;
            continue;
        }
        p = start;
    }
}

// ---------------------------------------------------------------------------
// Pass 2: emit everything: enums+flags+consts, c_struct types, functions.
// ---------------------------------------------------------------------------
static void emit_all(const char* s) {
    // ---- enums ----
    part_write(PART_ENUMS, "# ============================ enums ============================\n\n");
    for (const char* p = s; *p; p++) {
        if (starts_kw(p, "typedef") ) {
            const char* q = p + 7; skip_ws(&q);
            if (starts_kw(q, "enum")) {
                q += 4;
                const char* brace = strchr(q, '{');
                const char* close = brace ? match_brace(brace) : NULL;
                if (brace && close) {
                    const char* t = close + 1; char nm[96];
                    if (take_ident(&t, nm, sizeof(nm))) {
                        char body[16384]; size_t n = (size_t)(close - brace - 1);
                        if (n >= sizeof(body)) n = sizeof(body)-1;
                        memcpy(body, brace + 1, n); body[n] = '\0';
                        FILE* m = item_begin(); emit_enum(m, body, nm); item_end(m, PART_ENUMS); g_n_enum++;
                        p = close;
                    }
                }
            }
        }
    }
    // ---- flag constants (static const) ----
    part_write(PART_ENUMS, "# ======================= flags & constants ====================\n\n");
    for (const char* p = s; *p; p++) {
        if (starts_kw(p, "static")) {
            const char* q = p + 6; skip_ws(&q);
            if (starts_kw(q, "const")) {
                q += 5;
                const char* semi = strchr(q, ';');
                if (!semi) continue;
                const char* eq = memchr(q, '=', (size_t)(semi - q));
                if (!eq) continue;
                // name is last identifier before '='
                char name[96]; name[0] = '\0'; const char* t = q;
                while (t < eq) { char id[96]; if (take_ident(&t, id, sizeof(id))) snprintf(name, sizeof(name), "%s", id); else t++; }
                char raw[128]; size_t n = (size_t)(semi - eq - 1); if (n >= sizeof(raw)) n = sizeof(raw)-1;
                memcpy(raw, eq + 1, n); raw[n] = '\0';
                char val[64]; bool wide = false;
                char line[256];
                if (name[0] && parse_int_literal(raw, val, sizeof(val), &wide)) {
                    snprintf(line, sizeof(line), "const %s: UInt64 = %s\n", name, val); part_write(PART_ENUMS, line); g_n_flags++;
                } else if (name[0]) { snprintf(line, sizeof(line), "# skipped const %s (non-literal)\n", name); part_write(PART_ENUMS, line); skipped(name); }
                p = semi;
            }
        }
    }
    part_write(PART_ENUMS, "\n");
    // ---- #define constants ----
    size_t prefix_len = strlen(g_define_prefix);
    for (const char* p = s; *p; ) {
        if ((p == s || p[-1] == '\n') && p[0] == '#') {
            const char* q = p + 1; skip_ws(&q);
            if (starts_kw(q, "define")) {
                q += 6; char nm[128];
                if (take_ident(&q, nm, sizeof(nm)) && strncmp(nm, g_define_prefix, prefix_len) == 0 && *q != '(') {
                    // value = rest of line
                    const char* ls = q; const char* le = strchr(ls, '\n'); if (!le) le = ls + strlen(ls);
                    char raw[256]; size_t n = (size_t)(le - ls); if (n >= sizeof(raw)) n = sizeof(raw)-1;
                    memcpy(raw, ls, n); raw[n] = '\0';
                    char val[64]; bool wide = false;
                    if (parse_int_literal(raw, val, sizeof(val), &wide)) {
                        char line[256];
                        snprintf(line, sizeof(line), "const %s: %s = %s\n", nm, wide ? "UInt64" : "Int64", val);
                        part_write(PART_ENUMS, line); g_n_const++;
                    } else { skipped(nm); }
                }
            }
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }
        p++;
    }
    part_write(PART_TYPES, "# ============================ structs ==========================\n\n");
    // ---- structs ----
    for (const char* p = s; *p; p++) {
        if (starts_kw(p, "typedef")) {
            const char* q = p + 7; skip_ws(&q);
            if (starts_kw(q, "struct")) {
                const char* r = q + 6;
                const char* brace = strchr(r, '{');
                const char* semi = strchr(r, ';');
                if (brace && (!semi || brace < semi)) {
                    const char* close = match_brace(brace);
                    if (close) {
                        const char* t = close + 1; char nm[96];
                        if (take_ident(&t, nm, sizeof(nm))) {
                            char body[16384]; size_t n = (size_t)(close - brace - 1);
                            if (n >= sizeof(body)) n = sizeof(body)-1;
                            memcpy(body, brace + 1, n); body[n] = '\0';
                            FILE* m = item_begin(); emit_struct(m, body, nm); item_end(m, PART_TYPES); g_n_struct++;
                            p = close;
                        }
                    }
                }
            }
        }
    }
    // handles are mapped to Ptr; count them for the report.
    for (int i = 0; i < g_type_count; i++) { if (g_types[i].kind == TK_HANDLE) g_n_handle++; if (g_types[i].kind == TK_CALLBACK) g_n_callback++; }
    part_write(PART_FUNCS, "# ===========================  functions  =======================\n");
    part_write(PART_FUNCS, "# Handles are opaque Ptr; callbacks are Ptr; array/data pointers are\n");
    part_write(PART_FUNCS, "# Ptr (pass a List's .data + .length). Single struct pointers are\n");
    part_write(PART_FUNCS, "# view (const) / mod. See docs/webgpu-bindings.md.\n\n");
    // ---- functions ----
    if (g_api_macro || g_n_inline_macros > 0) {
        emit_marked_functions(s);
        return;
    }
    // Detect by the `wgpu<Name>(` pattern rather than WGPU_EXPORT: webgpu.h uses
    // WGPU_EXPORT, but wgpu-native's wgpu.h declares its functions plain. Every
    // WebGPU entry point is a lowercase-`wgpu`-prefixed identifier immediately
    // followed by `(`; function-pointer TYPES are `WGPU`-prefixed (uppercase) or
    // spelled `(*Name)`, so they don't match.
    for (const char* p = s; *p; ) {
        if (strncmp(p, "wgpu", 4) == 0 && isupper((unsigned char)p[4]) && (p == s || !is_ident_ch(p[-1]))) {
            const char* ne = p; while (is_ident_ch(*ne)) ne++;
            const char* lp = ne; while (*lp == ' ' || *lp == '\t') lp++;
            if (*lp == '(') {
                // matching ')'
                const char* rp = lp; int depth = 0;
                do { if (*rp == '(') depth++; else if (*rp == ')') depth--; rp++; } while (*rp && depth > 0);
                const char* aq = rp; while (*aq == ' ' || *aq == '\t' || *aq == '\n') aq++;
                if (*aq == ';') {
                    // return type spans back to the previous statement boundary
                    const char* rt = p;
                    while (rt > s && rt[-1] != ';' && rt[-1] != '}' && rt[-1] != '{') rt--;
                    char text[4096]; size_t n = (size_t)(rp - rt); if (n >= sizeof(text)) n = sizeof(text)-1;
                    memcpy(text, rt, n); text[n] = '\0';
                    FILE* m = item_begin(); emit_function(m, text); item_end(m, PART_FUNCS);
                    p = aq + 1; continue;
                }
            }
        }
        p++;
    }
}

// Entry point: <Module>Enums.rae (constants), <Module>Types.rae (c_struct
// mirrors) and <Module>.rae (functions), each continued in numbered parts
// when longer than the 1000-line cap.
int bindgen_run(int argc, char** argv) {
    const char* out_dir = NULL;
    const char* module = "webgpu";
    const char* import_prefix = NULL; // defaults to `module`
    const char* cheader = NULL;
    const char* headers[16]; int nheaders = 0;
    const char* module_comment = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--out-dir") && i + 1 < argc) out_dir = argv[++i];
        else if (!strcmp(argv[i], "--module") && i + 1 < argc) module = argv[++i];
        else if (!strcmp(argv[i], "--import-prefix") && i + 1 < argc) import_prefix = argv[++i];
        else if (!strcmp(argv[i], "--cheader") && i + 1 < argc) cheader = argv[++i];
        else if (!strcmp(argv[i], "--module-comment") && i + 1 < argc) module_comment = argv[++i];
        else if (!strcmp(argv[i], "--api-macro") && i + 1 < argc) g_api_macro = argv[++i];
        else if (!strcmp(argv[i], "--inline-macro") && i + 1 < argc) { if (g_n_inline_macros < 8) g_inline_macros[g_n_inline_macros++] = argv[i + 1]; i++; }
        else if (!strcmp(argv[i], "--drop-macro") && i + 1 < argc) { if (g_n_drop_macros < 16) g_drop_macros[g_n_drop_macros++] = argv[i + 1]; i++; }
        else if (!strcmp(argv[i], "--define-prefix") && i + 1 < argc) g_define_prefix = argv[++i];
        else if (!strcmp(argv[i], "--define") && i + 1 < argc) { bindgen_add_defined(argv[i + 1], strlen(argv[i + 1])); i++; }
        else if (!strcmp(argv[i], "--verbose")) g_verbose = true;
        else if (argv[i][0] == '-') { fprintf(stderr, "[bindgen] unknown option %s\n", argv[i]); return 1; }
        else if (nheaders < 16) headers[nheaders++] = argv[i];
    }
    if (nheaders == 0 || !out_dir) {
        fprintf(stderr, "usage: rae bindgen <header.h> [more.h ...] --out-dir <dir> [--module <name>] [--import-prefix <p>] [--cheader <include>] [--module-comment <text>] [--api-macro <M>] [--inline-macro <M>]... [--drop-macro <M>]... [--define-prefix <P>] [--verbose]\n");
        return 1;
    }
    if (!import_prefix) import_prefix = module;

    // Load + clean all headers into one buffer.
    size_t total = 0; char* combined = malloc(1); combined[0] = '\0';
    for (int i = 0; i < nheaders; i++) {
        size_t len = 0; char* raw = read_file(headers[i], &len);
        if (!raw) { free(combined); return 1; }
        strip_comments(raw); bindgen_preprocess_conditionals(raw); remove_macros(raw);
        combined = realloc(combined, total + len + 2);
        memcpy(combined + total, raw, len); total += len; combined[total++] = '\n'; combined[total] = '\0';
        free(raw);
    }

    classify(combined);
    register_aliases(combined);

    // Module FILES are PascalCase (`webgpu` -> `Webgpu`, #801/#818); the
    // package FOLDER stays the camelCase `import_prefix`.
    snprintf(g_pascal, sizeof(g_pascal), "%s", module);
    if (g_pascal[0] >= 'a' && g_pascal[0] <= 'z') g_pascal[0] = (char)(g_pascal[0] - 'a' + 'A');
    g_out_dir = out_dir; g_import_prefix = import_prefix; g_cheader = cheader; g_module_comment = module_comment;
    memset(g_parts, 0, sizeof(g_parts));

    emit_all(combined);
    for (int k = 0; k < 3; k++) if (g_parts[k].f) fclose(g_parts[k].f);
    free(combined);

    fprintf(stderr,
        "[bindgen] %s/{%sEnums,%sTypes,%s}.rae (%d/%d/%d parts): enums=%d flags=%d defines=%d structs=%d functions=%d handles=%d callbacks=%d skipped=%d\n",
        out_dir, g_pascal, g_pascal, g_pascal, g_parts[0].part, g_parts[1].part, g_parts[2].part,
        g_n_enum, g_n_flags, g_n_const, g_n_struct, g_n_func, g_n_handle, g_n_callback, g_n_skipped);
    return 0;
}
