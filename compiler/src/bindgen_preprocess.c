// The #if evaluation of `rae bindgen` (bindgen.c): a header's alternative
// branches (BOX3D_DOUBLE_PRECISION, __cplusplus) must not both reach the
// declaration parser. Only `defined(X)`, `!`, `&&`, `||`, `0` and `1` are
// understood; a branch whose condition holds anything else is kept.
#include "bindgen_preprocess.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Macros taken as defined (--define, plus every #define met in an active
// region).
static char g_defined[128][96];
static int g_n_defined = 0;

static bool is_ident_ch(char c) { return isalnum((unsigned char)c) || c == '_'; }

static bool is_defined(const char* name, size_t len) {
    for (int i = 0; i < g_n_defined; i++)
        if (strlen(g_defined[i]) == len && strncmp(g_defined[i], name, len) == 0) return true;
    return false;
}

void bindgen_add_defined(const char* name, size_t len) {
    if (g_n_defined >= 128 || len == 0 || len >= sizeof(g_defined[0]) || is_defined(name, len)) return;
    memcpy(g_defined[g_n_defined], name, len); g_defined[g_n_defined][len] = '\0'; g_n_defined++;
}

// Evaluate an #if expression made of defined(X) / defined X, !, &&, ||, 0
// and 1. Returns -1 when it holds anything else (the caller keeps the
// branch: an unknown condition is not a reason to drop declarations).
static int eval_condition(const char* e, const char* end) {
    int result = -1, pending_op = 0; // 0 none, 1 and, 2 or
    while (e < end) {
        while (e < end && isspace((unsigned char)*e)) e++;
        if (e >= end) break;
        bool negate = false;
        while (e < end && *e == '!') { negate = !negate; e++; while (e < end && isspace((unsigned char)*e)) e++; }
        int value;
        if (end - e >= 7 && strncmp(e, "defined", 7) == 0) {
            e += 7; while (e < end && (isspace((unsigned char)*e) || *e == '(')) e++;
            const char* ns = e; while (e < end && is_ident_ch(*e)) e++;
            value = is_defined(ns, (size_t)(e - ns)) ? 1 : 0;
            while (e < end && (isspace((unsigned char)*e) || *e == ')')) e++;
        } else if (*e == '0' || *e == '1') {
            value = *e - '0'; e++;
        } else return -1;
        if (negate) value = !value;
        if (pending_op == 0) result = value;
        else if (pending_op == 1) result = result && value;
        else result = result || value;
        while (e < end && isspace((unsigned char)*e)) e++;
        if (e >= end) break;
        if (end - e >= 2 && e[0] == '&' && e[1] == '&') { pending_op = 1; e += 2; }
        else if (end - e >= 2 && e[0] == '|' && e[1] == '|') { pending_op = 2; e += 2; }
        else return -1;
    }
    return result;
}

// Blank out the inactive branches of #if/#ifdef/#ifndef/#elif/#else/#endif,
// so a header's alternatives (BOX3D_DOUBLE_PRECISION, __cplusplus) do not
// both reach the parser. Newlines are kept.
void bindgen_preprocess_conditionals(char* s) {
    // Per nesting level: is the enclosing region active, has a branch of this
    // #if been taken.
    bool outer[64], taken[64]; int depth = 0;
    bool live = true;
    for (char* line = s; *line; ) {
        char* eol = strchr(line, '\n'); if (!eol) eol = line + strlen(line);
        const char* d = line; while (d < eol && (*d == ' ' || *d == '\t')) d++;
        bool directive = d < eol && *d == '#';
        if (directive) {
            d++; while (d < eol && (*d == ' ' || *d == '\t')) d++;
            const char* w = d; while (d < eol && is_ident_ch(*d)) d++;
            size_t wl = (size_t)(d - w);
            #define DIR_IS(lit) (wl == strlen(lit) && strncmp(w, lit, wl) == 0)
            if (DIR_IS("if") || DIR_IS("ifdef") || DIR_IS("ifndef")) {
                int cond;
                if (DIR_IS("if")) cond = eval_condition(d, eol);
                else {
                    while (d < eol && isspace((unsigned char)*d)) d++;
                    const char* ns = d; while (d < eol && is_ident_ch(*d)) d++;
                    cond = is_defined(ns, (size_t)(d - ns)) ? 1 : 0;
                    if (DIR_IS("ifndef")) cond = !cond;
                }
                if (depth < 64) { outer[depth] = live; taken[depth] = cond != 0; depth++; }
                live = live && cond != 0;
            } else if ((DIR_IS("elif") || DIR_IS("else")) && depth > 0) {
                int cond = DIR_IS("else") ? 1 : eval_condition(d, eol);
                bool now = !taken[depth - 1] && cond != 0;
                if (cond == -1) now = true;
                if (now) taken[depth - 1] = true;
                live = outer[depth - 1] && now;
            } else if (DIR_IS("endif") && depth > 0) {
                depth--; live = outer[depth];
            } else if (DIR_IS("define") && live) {
                while (d < eol && isspace((unsigned char)*d)) d++;
                const char* ns = d; while (d < eol && is_ident_ch(*d)) d++;
                bindgen_add_defined(ns, (size_t)(d - ns));
            }
            #undef DIR_IS
        }
        if (!live || (directive && !live)) { for (char* c = line; c < eol; c++) *c = ' '; }
        line = *eol ? eol + 1 : eol;
    }
}

