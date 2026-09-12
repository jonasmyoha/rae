/* pretty_internal.h - shared state of the Rae formatter (split by #916)
 *
 *   pretty.c        entry point + module traversal (imports, decls, raefmt off/on)
 *   pretty_writer.c the output buffer: indent, width, blank lines, comments
 *   pretty_expr.c   types + expressions + the ONE list policy (args, literals)
 *   pretty_decl.c   statements + declarations
 *
 * Policy (docs/rae-format-design.md 'Decisions (#911)'): 2-space indent,
 * PP_MAX_WIDTH columns, parameters/arguments vertical from PP_WRAP_ARGS items
 * or when the one-line form would not fit, type fields / object / collection
 * literals from PP_WRAP_FIELDS; vertical lists have one item per line, no
 * commas, and the closing delimiter back at the opening line's indent.
 */

#ifndef PRETTY_INTERNAL_H
#define PRETTY_INTERNAL_H

#include <stdbool.h>
#include <stdio.h>

#include "ast.h"
#include "lexer.h"

#define PP_MAX_WIDTH 100
#define PP_WRAP_ARGS 4    /* parameters, arguments, return items/values */
#define PP_WRAP_FIELDS 5  /* type fields, enum members, object/collection/list literals */
#define PP_MAX_VERBATIM 64

typedef struct {
  size_t start_line;
  size_t end_line;
} VerbatimRange;

typedef struct {
  FILE* out;                 /* NULL while measuring */
  int indent;
  int start_of_line;
  int current_col;
  bool last_line_blank;      /* the line just ended was empty (caps blank lines at one) */
  bool block_start;          /* the next statement opens its block: no blank line before it */
  const Token* comments;
  size_t comment_count;
  size_t next_comment_idx;
  const char* source;
  VerbatimRange verbatim_ranges[PP_MAX_VERBATIM];
  size_t verbatim_count;
  size_t lines_written;      /* newlines emitted so far (a statement that spans lines gets its trailing comment below it) */
  int stmt_indent;           /* indent of the statement being printed */
  size_t stmt_line;          /* first source line of the statement being printed: a comment there is the STATEMENT's trailer, not an item's */
  bool measuring;            /* dry run of a one-line rendering: newlines count as a space */
  bool forced_break;         /* set in a dry run when a nested list had to go vertical */
} PrettyPrinter;

/* ---- writer (pretty_writer.c) ---- */
void pp_write_raw(PrettyPrinter* pp, const char* text, size_t len);
void pp_write(PrettyPrinter* pp, const char* text);
void pp_write_char(PrettyPrinter* pp, char ch);
void pp_write_str(PrettyPrinter* pp, Str s);
void pp_space(PrettyPrinter* pp);
void pp_newline(PrettyPrinter* pp);
void pp_blank_line(PrettyPrinter* pp);
void pp_begin_block(PrettyPrinter* pp);
void pp_end_block(PrettyPrinter* pp, size_t end_line);
void pp_write_string_literal(PrettyPrinter* pp, Str s);
void pp_write_string_body(PrettyPrinter* pp, Str s);
void pp_write_raw_string_literal(PrettyPrinter* pp, Str s);
/* Begin a dry run on a copy of `pp`; the copy writes nothing and reports the
 * one-line width in current_col (+ forced_break when a list had to wrap). */
PrettyPrinter pp_measure_begin(const PrettyPrinter* pp);
/* Comments: everything before `line` (blank-line aware), the trailing comment
 * on `line` itself (after the statement's text), and the source's blank line
 * before `line` (at most one is kept). */
void pp_flush_comments_before(PrettyPrinter* pp, size_t line);
void pp_flush_trailing_comment(PrettyPrinter* pp, size_t line, size_t lines_before);
void pp_blank_line_before(PrettyPrinter* pp, size_t line);
bool pp_source_line_is_blank(const PrettyPrinter* pp, size_t line);
/* raefmt: off / on */
VerbatimRange* pp_find_verbatim_range(PrettyPrinter* pp, size_t line);
void pp_print_verbatim_range(PrettyPrinter* pp, size_t start_line, size_t end_line);

/* ---- the list policy + expressions (pretty_expr.c) ---- */
typedef void (*PpItemsFn)(PrettyPrinter* pp, const void* items);
typedef struct {
  int threshold;      /* vertical from this many items */
  const char* open;   /* "(" / "{" / "[" */
  const char* close;
  bool pad;           /* "{ a, b }" (true) vs "(a, b)" (false) in the one-line form */
  int tail_width;     /* exact width of what follows `close` on the same line, when known */
  const char* open_vertical;  /* the opener of the vertical form when it differs (NULL: `open`) */
  bool never_vertical;        /* `ret a, b`: the grammar has no vertical form, stay on one line */
} PpListStyle;
/* Prints open + items + close: one line when count < threshold AND the
 * one-line form (from the current column, plus tail_width) fits PP_MAX_WIDTH;
 * otherwise one item per line, two spaces deeper, close at the opening indent. */
void pp_print_list(PrettyPrinter* pp, const void* items, int count, PpListStyle style,
                   PpItemsFn oneline, PpItemsFn vertical);
void pp_write_type(PrettyPrinter* pp, const AstTypeRef* type);
void pp_write_properties(PrettyPrinter* pp, const AstProperty* prop, const char* extern_symbol);
void pp_expr(PrettyPrinter* pp, const AstExpr* expr);
void pp_call_args(PrettyPrinter* pp, const AstCallArg* args);

/* ---- statements + declarations (pretty_decl.c) ---- */
void pp_print_block_body(PrettyPrinter* pp, const AstBlock* block);
void pp_print_decl(PrettyPrinter* pp, const AstDecl* decl);

#endif /* PRETTY_INTERNAL_H */
