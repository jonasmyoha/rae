/* pretty_writer.c - the formatter's output buffer: indentation, column
 * tracking, blank lines, comments and raefmt off/on verbatim ranges (#916). */

#include "pretty_internal.h"

#include <ctype.h>
#include <string.h>

/* Columns are code points, not bytes: an em dash in a comment is one column. */
static int utf8_columns(const char* text, size_t len) {
  int cols = 0;
  for (size_t i = 0; i < len; i++) {
    if (((unsigned char)text[i] & 0xC0) != 0x80) cols++;
  }
  return cols;
}

static void pp_write_indent(PrettyPrinter* pp) {
  if (pp->measuring) return;
  for (int i = 0; i < pp->indent; ++i) {
    if (pp->out) fputs("  ", pp->out);
    pp->current_col += 2;
  }
}

void pp_write_raw(PrettyPrinter* pp, const char* text, size_t len) {
  if (len == 0) return;
  if (pp->start_of_line) {
    pp_write_indent(pp);
    pp->start_of_line = 0;
  }
  if (pp->out) fwrite(text, 1, len, pp->out);
  pp->current_col += utf8_columns(text, len);
}

void pp_write(PrettyPrinter* pp, const char* text) {
  pp_write_raw(pp, text, strlen(text));
}

void pp_write_char(PrettyPrinter* pp, char ch) {
  pp_write_raw(pp, &ch, 1);
}

void pp_write_str(PrettyPrinter* pp, Str s) {
  if (!s.data || s.len == 0) return;
  pp_write_raw(pp, s.data, s.len);
}

void pp_space(PrettyPrinter* pp) {
  pp_write_char(pp, ' ');
}

void pp_newline(PrettyPrinter* pp) {
  if (pp->measuring) {
    /* A dry run renders on one line; a break is worth the space it becomes. */
    pp->current_col += 1;
    return;
  }
  if (pp->out) fputc('\n', pp->out);
  pp->lines_written++;
  pp->last_line_blank = pp->start_of_line != 0;
  pp->start_of_line = 1;
  pp->current_col = 0;
}

/* At most one blank line in a row, never at the start of a block. */
void pp_blank_line(PrettyPrinter* pp) {
  if (pp->measuring || pp->block_start) return;
  if (!pp->start_of_line) pp_newline(pp);
  if (!pp->last_line_blank) pp_newline(pp);
}

void pp_begin_block(PrettyPrinter* pp) {
  pp_write_char(pp, '{');
  pp_newline(pp);
  pp->indent += 1;
  pp->block_start = true;
}

/* Comments left between the last statement and the `}` stay inside the block. */
void pp_end_block(PrettyPrinter* pp, size_t end_line) {
  if (end_line > 0) pp_flush_comments_before(pp, end_line);
  pp->indent -= 1;
  pp->block_start = false;
  if (!pp->start_of_line) pp_newline(pp);
  pp_write_char(pp, '}');
}

/* The escapes the lexer understands; `{`/`}` MUST be escaped in a plain
 * string because a bare `{` starts an interpolation. */
void pp_write_string_body(PrettyPrinter* pp, Str s) {
  for (size_t i = 0; i < s.len; i++) {
    char c = s.data[i];
    switch (c) {
      case '"': pp_write(pp, "\\\""); break;
      case '\\': pp_write(pp, "\\\\"); break;
      case '\n': pp_write(pp, "\\n"); break;
      case '\r': pp_write(pp, "\\r"); break;
      case '\t': pp_write(pp, "\\t"); break;
      case '\0': pp_write(pp, "\\0"); break;
      case '{': pp_write(pp, "\\{"); break;
      case '}': pp_write(pp, "\\}"); break;
      default: pp_write_char(pp, c); break;
    }
  }
}

void pp_write_string_literal(PrettyPrinter* pp, Str s) {
  pp_write_char(pp, '"');
  pp_write_string_body(pp, s);
  pp_write_char(pp, '"');
}

/* r"..." needs as many `#` as it takes for no `"#…` inside to close it early. */
static int count_required_hashes(Str s) {
  int n = 0;
  for (;;) {
    bool found = false;
    for (size_t i = 0; i < s.len; i++) {
      if (s.data[i] != '"') continue;
      int hashes = 0;
      size_t j = i + 1;
      while (j < s.len && s.data[j] == '#') { hashes++; j++; }
      if (hashes >= n) { found = true; break; }
    }
    if (!found) return n;
    n++;
  }
}

void pp_write_raw_string_literal(PrettyPrinter* pp, Str s) {
  int hashes = count_required_hashes(s);
  pp_write(pp, "r");
  for (int i = 0; i < hashes; i++) pp_write(pp, "#");
  pp_write(pp, "\"");
  pp_write_str(pp, s);
  pp_write(pp, "\"");
  for (int i = 0; i < hashes; i++) pp_write(pp, "#");
}

PrettyPrinter pp_measure_begin(const PrettyPrinter* pp) {
  PrettyPrinter measure = *pp;
  measure.out = NULL;
  measure.measuring = true;
  measure.forced_break = false;
  return measure;
}

/* ---- source lines ---- */

static const char* get_line_ptr(const char* source, size_t line) {
  if (!source) return NULL;
  const char* p = source;
  size_t current = 1;
  while (current < line && *p) {
    if (*p == '\n') current++;
    p++;
  }
  return p;
}

bool pp_source_line_is_blank(const PrettyPrinter* pp, size_t line) {
  const char* p = get_line_ptr(pp->source, line);
  if (!p) return false;
  while (*p && *p != '\n') {
    if (!isspace((unsigned char)*p)) return false;
    p++;
  }
  return true;
}

void pp_blank_line_before(PrettyPrinter* pp, size_t line) {
  if (pp->block_start || line <= 1) return;
  if (pp_source_line_is_blank(pp, line - 1)) pp_blank_line(pp);
}

void pp_print_verbatim_range(PrettyPrinter* pp, size_t start_line, size_t end_line) {
  const char* start = get_line_ptr(pp->source, start_line);
  const char* end = get_line_ptr(pp->source, end_line + 1);
  if (start && end && end > start) {
    if (pp->out) fwrite(start, 1, (size_t)(end - start), pp->out);
    pp->start_of_line = 1;
    pp->last_line_blank = false;
    pp->current_col = 0;
  }
}

VerbatimRange* pp_find_verbatim_range(PrettyPrinter* pp, size_t line) {
  for (size_t i = 0; i < pp->verbatim_count; i++) {
    if (line >= pp->verbatim_ranges[i].start_line && line <= pp->verbatim_ranges[i].end_line) {
      return &pp->verbatim_ranges[i];
    }
  }
  return NULL;
}

/* ---- comments ---- */

/* A `# ...` line is wrapped to PP_MAX_WIDTH at a space when it has one in
 * range; a run with no break point (a URL, a symbol, a diagram row) is left
 * as it is — the width is a layout target, never a reason to damage data.
 * Comments that do not start with `# ` (`#!`, `#raefmt`, `#---`) are copied
 * verbatim too. */
static void pp_write_line_comment(PrettyPrinter* pp, Str lexeme) {
  const char* text = lexeme.data;
  size_t len = lexeme.len;
  while (len > 0 && isspace((unsigned char)text[len - 1])) len--;
  if (len < 2 || text[0] != '#' || text[1] != ' ') {
    pp_write_raw(pp, text, len);
    pp_newline(pp);
    return;
  }
  /* Body after "# "; a further indent (`#   - item`) is kept on the first
   * line and repeated on continuation lines. */
  size_t body = 2;
  while (body < len && text[body] == ' ') body++;
  size_t hang = body - 2;
  int available = PP_MAX_WIDTH - pp->indent * 2 - 2 - (int)hang;
  if (available < 20) available = 20;
  size_t pos = body;
  while (pos < len) {
    size_t remaining = len - pos;
    size_t take = remaining;
    if (utf8_columns(text + pos, remaining) > available) {
      /* Walk to the last space that keeps the line within `available`. */
      size_t best = 0;
      int cols = 0;
      for (size_t i = 0; i < remaining; i++) {
        if (((unsigned char)text[pos + i] & 0xC0) != 0x80) cols++;
        if (cols > available) break;
        if (text[pos + i] == ' ') best = i;
      }
      if (best > 0) take = best;
    }
    pp_write(pp, "# ");
    for (size_t i = 0; i < hang; i++) pp_write_char(pp, ' ');
    pp_write_raw(pp, text + pos, take);
    pp_newline(pp);
    pos += take;
    while (pos < len && text[pos] == ' ') pos++;
  }
}

void pp_flush_comments_before(PrettyPrinter* pp, size_t line) {
  if (pp->measuring) return;
  while (pp->next_comment_idx < pp->comment_count &&
         pp->comments[pp->next_comment_idx].line < line) {
    const Token* comment = &pp->comments[pp->next_comment_idx++];
    if (pp_find_verbatim_range(pp, comment->line)) continue;
    if (!pp->start_of_line) pp_newline(pp);
    pp_blank_line_before(pp, comment->line);
    if (comment->kind == TOK_COMMENT) {
      pp_write_line_comment(pp, comment->lexeme);
    } else {
      pp_write_str(pp, comment->lexeme);
      pp_newline(pp);
    }
    pp->block_start = false;
  }
}

/* `x = 1  # note` — the comment that shares the statement's first line
 * follows the statement's text when the statement stayed on one line
 * (`lines_before` is pp->lines_written from before the statement). A
 * statement that went vertical gets it on its own line below, which is
 * where the next pass will find it — so both spellings are stable. */
void pp_flush_trailing_comment(PrettyPrinter* pp, size_t line, size_t lines_before) {
  if (pp->measuring) return;
  /* An item inside a vertical list that opened on the statement's first line
   * must not claim the statement's own trailing comment (`foo(a: 1, ...) # x`
   * → the `# x` goes below the whole call, not after `a: 1`). */
  if (pp->indent > pp->stmt_indent && line == pp->stmt_line) return;
  while (pp->next_comment_idx < pp->comment_count &&
         pp->comments[pp->next_comment_idx].line == line) {
    const Token* comment = &pp->comments[pp->next_comment_idx++];
    if (pp_find_verbatim_range(pp, comment->line)) continue;
    Str lexeme = comment->lexeme;
    while (lexeme.len > 0 && isspace((unsigned char)lexeme.data[lexeme.len - 1])) lexeme.len--;
    if (pp->start_of_line || pp->lines_written != lines_before) {
      if (!pp->start_of_line) pp_newline(pp);
      pp_write_str(pp, lexeme);
      pp_newline(pp);
    } else {
      pp_write(pp, "  ");
      pp_write_str(pp, lexeme);
    }
  }
}
