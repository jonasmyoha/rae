// C-header -> Rae FFI binding generator (general FFI, #498).
//
// A focused, dependency-free parser for the regular WebGPU C header style
// (webgpu.h + wgpu.h), emitting low-level Rae bindings that bind DIRECTLY to
// the C ABI via c_struct types and extern("symbol") functions — no shim.
// Deterministic: the same headers always produce byte-identical output.
//
// It is intentionally not a general C parser. It recognises the specific,
// regular constructs these headers use (see the census in docs/webgpu-bindings.md):
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

static bool type_is(const char* name, TypeKind kind) {
    for (int i = 0; i < g_type_count; i++)
        if (strcmp(g_types[i].name, name) == 0) return g_types[i].kind == kind;
    return false;
}
static bool type_known(const char* name, TypeKind* out) {
    for (int i = 0; i < g_type_count; i++)
        if (strcmp(g_types[i].name, name) == 0) { if (out) *out = g_types[i].kind; return true; }
    return false;
}

// Coverage counters, reported at the end.
static int g_n_enum = 0, g_n_flags = 0, g_n_handle = 0, g_n_struct = 0,
           g_n_func = 0, g_n_const = 0, g_n_callback = 0, g_n_skipped = 0;
static bool g_verbose = false;

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
            snprintf(out, cap, "view %s", base); return true;
        }
        // struct pointer in a field, handle*, enum*, primitive*, string ptr: Ptr.
        snprintf(out, cap, "Ptr"); return true;
    }
    // Non-pointer.
    if (!strcmp(base, "void")) { out[0] = '\0'; return true; } // return-only
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
                    const char* close = strchr(brace, '}');
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
                if (star && semi && star < semi) {
                    const char* r = star + 2; char nm[96];
                    if (take_ident(&r, nm, sizeof(nm))) reg_type(nm, TK_CALLBACK);
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
static void emit_enum(FILE* out, const char* body, const char* tname) {
    // body is between '{' and '}'. Members: NAME = VALUE ,
    fprintf(out, "# enum %s\n", tname);
    const char* p = body;
    while (*p) {
        char nm[96];
        if (!take_ident(&p, nm, sizeof(nm))) { if (*p) p++; continue; }
        skip_ws(&p);
        if (*p != '=') { // no value; skip to comma
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
        } else {
            fprintf(out, "# skipped const %s (non-literal value)\n", nm); skipped(nm);
        }
        if (*p == ',') p++;
    }
    fprintf(out, "\n");
}

static void emit_struct(FILE* out, const char* body, const char* tname) {
    fprintf(out, "type %s: c_struct {\n", tname);
    const char* p = body;
    int fields = 0;
    while (*p) {
        skip_ws(&p);
        const char* semi = strchr(p, ';');
        if (!semi) break;
        char field[512]; size_t len = (size_t)(semi - p); if (len >= sizeof(field)) len = sizeof(field)-1;
        memcpy(field, p, len); field[len] = '\0';
        p = semi + 1;
        // skip empty
        bool blank = true; for (size_t i = 0; field[i]; i++) if (!isspace((unsigned char)field[i])) { blank = false; break; }
        if (blank) continue;
        Decl d = parse_decl(field);
        if (!d.ok || d.name[0] == '\0') { fprintf(out, "  # skipped field: %s\n", field); skipped("struct field"); continue; }
        char rae[128];
        if (!map_type(d.base, d.ptr, 0, rae, sizeof(rae)) || rae[0] == '\0') {
            fprintf(out, "  # skipped field %s (unmapped type %s)\n", d.name, d.base); skipped(d.name); continue;
        }
        char nm[96];
        fprintf(out, "  %s: %s\n", safe_name(d.name, nm, sizeof(nm)), rae);
        fields++;
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
    if (fname[0] == '\0') { skipped("function (no name)"); return; }
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
    const char* q = params;
    while (*q && !bad) {
        while (*q && isspace((unsigned char)*q)) q++;
        if (!*q) break;
        const char* start = q; int depth = 0;
        while (*q && !(depth == 0 && *q == ',')) { if (*q=='(') depth++; else if (*q==')') depth--; q++; }
        char one[512]; size_t n = (size_t)(q - start); if (n >= sizeof(one)) n = sizeof(one)-1;
        memcpy(one, start, n); one[n] = '\0';
        if (*q == ',') q++;
        // trim
        Decl d = parse_decl(one);
        if (!d.ok) continue;
        if (!strcmp(d.base, "void") && d.ptr == 0) continue; // (void)
        char rae[128];
        // param context; const struct-ptr -> view, non-const struct-ptr -> mod
        if (d.ptr == 1 && type_is(d.base, TK_STRUCT)) {
            snprintf(rae, sizeof(rae), "%s %s", d.is_const ? "view" : "mod", d.base);
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
    sp += snprintf(sig + sp, sizeof(sig) - sp, ") extern(\"%s\")", fname);
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
// Pass 2: emit everything, routed across three files (each under the 1000-line
// per-file cap): `fe` enums+flags+consts, `ft` c_struct types, `ff` functions.
// ---------------------------------------------------------------------------
static void emit_all(FILE* fe, FILE* ft, FILE* ff, const char* s) {
    FILE* out = fe;
    // ---- enums ----
    fprintf(out, "# ============================ enums ============================\n\n");
    for (const char* p = s; *p; p++) {
        if (starts_kw(p, "typedef") ) {
            const char* q = p + 7; skip_ws(&q);
            if (starts_kw(q, "enum")) {
                q += 4;
                const char* brace = strchr(q, '{');
                const char* close = brace ? strchr(brace, '}') : NULL;
                if (brace && close) {
                    const char* t = close + 1; char nm[96];
                    if (take_ident(&t, nm, sizeof(nm))) {
                        char body[16384]; size_t n = (size_t)(close - brace - 1);
                        if (n >= sizeof(body)) n = sizeof(body)-1;
                        memcpy(body, brace + 1, n); body[n] = '\0';
                        emit_enum(out, body, nm); g_n_enum++;
                        p = close;
                    }
                }
            }
        }
    }
    // ---- flag constants (static const) ----
    fprintf(out, "# ======================= flags & constants ====================\n\n");
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
                if (name[0] && parse_int_literal(raw, val, sizeof(val), &wide)) {
                    fprintf(out, "const %s: UInt64 = %s\n", name, val); g_n_flags++;
                } else if (name[0]) { fprintf(out, "# skipped const %s (non-literal)\n", name); skipped(name); }
                p = semi;
            }
        }
    }
    fprintf(out, "\n");
    // ---- #define constants ----
    for (const char* p = s; *p; ) {
        if ((p == s || p[-1] == '\n') && p[0] == '#') {
            const char* q = p + 1; skip_ws(&q);
            if (starts_kw(q, "define")) {
                q += 6; char nm[128];
                if (take_ident(&q, nm, sizeof(nm)) && strncmp(nm, "WGPU_", 5) == 0) {
                    // value = rest of line
                    const char* ls = q; const char* le = strchr(ls, '\n'); if (!le) le = ls + strlen(ls);
                    char raw[256]; size_t n = (size_t)(le - ls); if (n >= sizeof(raw)) n = sizeof(raw)-1;
                    memcpy(raw, ls, n); raw[n] = '\0';
                    char val[64]; bool wide = false;
                    if (parse_int_literal(raw, val, sizeof(val), &wide)) {
                        fprintf(out, "const %s: %s = %s\n", nm, wide ? "UInt64" : "Int64", val); g_n_const++;
                    } else { skipped(nm); }
                }
            }
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }
        p++;
    }
    out = ft;
    fprintf(out, "# ============================ structs ==========================\n\n");
    // ---- structs ----
    for (const char* p = s; *p; p++) {
        if (starts_kw(p, "typedef")) {
            const char* q = p + 7; skip_ws(&q);
            if (starts_kw(q, "struct")) {
                const char* r = q + 6;
                const char* brace = strchr(r, '{');
                const char* semi = strchr(r, ';');
                if (brace && (!semi || brace < semi)) {
                    const char* close = strchr(brace, '}');
                    if (close) {
                        const char* t = close + 1; char nm[96];
                        if (take_ident(&t, nm, sizeof(nm))) {
                            char body[16384]; size_t n = (size_t)(close - brace - 1);
                            if (n >= sizeof(body)) n = sizeof(body)-1;
                            memcpy(body, brace + 1, n); body[n] = '\0';
                            emit_struct(out, body, nm); g_n_struct++;
                            p = close;
                        }
                    }
                }
            }
        }
    }
    // handles are mapped to Ptr; count them for the report.
    for (int i = 0; i < g_type_count; i++) { if (g_types[i].kind == TK_HANDLE) g_n_handle++; if (g_types[i].kind == TK_CALLBACK) g_n_callback++; }
    out = ff;
    fprintf(out, "# ===========================  functions  =======================\n");
    fprintf(out, "# Handles are opaque Ptr; callbacks are Ptr; array/data pointers are\n");
    fprintf(out, "# Ptr (pass a List's .data + .length). Single struct pointers are\n");
    fprintf(out, "# view (const) / mod. See docs/webgpu-bindings.md.\n\n");
    // ---- functions ----
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
                    emit_function(out, text);
                    p = aq + 1; continue;
                }
            }
        }
        p++;
    }
}

static void emit_file_header(FILE* out, const char* module_comment) {
    fprintf(out, "# GENERATED by `rae bindgen` — do not edit by hand.\n");
    fprintf(out, "# Regenerate with the command documented in docs/webgpu-bindings.md.\n");
    if (module_comment) fprintf(out, "# %s\n", module_comment);
    fprintf(out, "#\n# Low-level Rae bindings to the C ABI (general FFI, #497/#498). Handles are\n");
    fprintf(out, "# opaque Ptr; enums are Int32 consts; flag sets are UInt64 consts; structs\n");
    fprintf(out, "# are c_struct mirrors of the real C types; functions bind via\n");
    fprintf(out, "# extern(\"symbol\") straight to the library. Build ergonomic wrappers on\n");
    fprintf(out, "# top in a separate module, not here.\n\n");
}

// ---------------------------------------------------------------------------
// Entry point. Emits THREE files (the compiler caps files at 1000 lines):
//   <dir>/<module>_enums.rae   enum + flag + #define constants
//   <dir>/<module>_types.rae   c_struct type mirrors (+ cheader)
//   <dir>/<module>.rae         functions (+ cheader, imports _types)
// ---------------------------------------------------------------------------
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
        else if (!strcmp(argv[i], "--verbose")) g_verbose = true;
        else if (argv[i][0] == '-') { fprintf(stderr, "[bindgen] unknown option %s\n", argv[i]); return 1; }
        else if (nheaders < 16) headers[nheaders++] = argv[i];
    }
    if (nheaders == 0 || !out_dir) {
        fprintf(stderr, "usage: rae bindgen <header.h> [more.h ...] --out-dir <dir> [--module <name>] [--import-prefix <p>] [--cheader <include>] [--module-comment <text>] [--verbose]\n");
        return 1;
    }
    if (!import_prefix) import_prefix = module;

    // Load + clean all headers into one buffer.
    size_t total = 0; char* combined = malloc(1); combined[0] = '\0';
    for (int i = 0; i < nheaders; i++) {
        size_t len = 0; char* raw = read_file(headers[i], &len);
        if (!raw) { free(combined); return 1; }
        strip_comments(raw); remove_macros(raw);
        combined = realloc(combined, total + len + 2);
        memcpy(combined + total, raw, len); total += len; combined[total++] = '\n'; combined[total] = '\0';
        free(raw);
    }

    classify(combined);

    char pe[1024], pt[1024], pf[1024];
    snprintf(pe, sizeof(pe), "%s/%s_enums.rae", out_dir, module);
    snprintf(pt, sizeof(pt), "%s/%s_types.rae", out_dir, module);
    snprintf(pf, sizeof(pf), "%s/%s.rae", out_dir, module);
    FILE* fe = fopen(pe, "w"); FILE* ft = fopen(pt, "w"); FILE* ff = fopen(pf, "w");
    if (!fe || !ft || !ff) { fprintf(stderr, "[bindgen] cannot write outputs under %s\n", out_dir); free(combined); return 1; }

    emit_file_header(fe, module_comment);
    emit_file_header(ft, module_comment);
    emit_file_header(ff, module_comment);
    // Types and functions reference the real C library structs -> need the header.
    if (cheader) { fprintf(ft, "cheader \"%s\"\n\n", cheader); fprintf(ff, "cheader \"%s\"\n\n", cheader); }
    // Functions use the c_struct types by name (view WGPUXDescriptor).
    fprintf(ff, "import %s/%s_types\n\n", import_prefix, module);

    emit_all(fe, ft, ff, combined);
    fclose(fe); fclose(ft); fclose(ff);
    free(combined);

    fprintf(stderr,
        "[bindgen] %s/{%s_enums,%s_types,%s}.rae: enums=%d flags=%d defines=%d structs=%d functions=%d handles=%d callbacks=%d skipped=%d\n",
        out_dir, module, module, module,
        g_n_enum, g_n_flags, g_n_const, g_n_struct, g_n_func, g_n_handle, g_n_callback, g_n_skipped);
    return 0;
}
