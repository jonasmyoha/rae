/* pretty.c - Rae formatter entry point: module traversal (imports, C headers,
 * declarations) and the `# raefmt: off` / `# raefmt: on` verbatim ranges.
 * The writer, expressions and declarations live in pretty_writer.c,
 * pretty_expr.c and pretty_decl.c (split by #916). */

#include "pretty.h"
#include "pretty_internal.h"

#include <string.h>

/* An import path is written bare (`ui/renderSystem/RenderSystem`) unless it
 * holds something the bare spelling cannot carry (a file suffix, a space). */
static bool import_path_needs_quotes(Str path) {
  for (size_t i = 0; i < path.len; i++) {
    char c = path.data[i];
    bool bare = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '/';
    if (!bare) return true;
  }
  return path.len == 0;
}

static void pp_print_import(PrettyPrinter* pp, const AstImport* imp) {
  pp_flush_comments_before(pp, imp->line);
  pp_blank_line_before(pp, imp->line);
  pp_write(pp, imp->is_export ? "export " : imp->is_open ? "open " : "import ");
  if (import_path_needs_quotes(imp->path)) {
    pp_write_string_literal(pp, imp->path);
  } else {
    pp_write_str(pp, imp->path);
  }
  if (imp->alias.len > 0) {
    pp_write(pp, " as ");
    pp_write_str(pp, imp->alias);
  }
  pp_flush_trailing_comment(pp, imp->line, pp->lines_written);
  pp_newline(pp);
}

static void collect_verbatim_ranges(PrettyPrinter* pp, const AstModule* module) {
  size_t off_line = 0;
  for (size_t i = 0; i < module->comment_count; i++) {
    const Token* c = &module->comments[i];
    if (c->kind != TOK_COMMENT) continue;
    if (strstr(c->lexeme.data, "raefmt: off")) {
      if (off_line == 0) off_line = c->line;
    } else if (strstr(c->lexeme.data, "raefmt: on")) {
      if (off_line != 0) {
        if (pp->verbatim_count < PP_MAX_VERBATIM) {
          pp->verbatim_ranges[pp->verbatim_count++] = (VerbatimRange){off_line, c->line};
        }
        off_line = 0;
      }
    }
  }
  if (off_line != 0 && pp->verbatim_count < PP_MAX_VERBATIM) {
    pp->verbatim_ranges[pp->verbatim_count++] = (VerbatimRange){off_line, (size_t)-1};
  }
}

void pretty_print_module(const AstModule* module, const char* source, FILE* out) {
  if (!module) return;
  PrettyPrinter pp = {
      .out = out,
      .indent = 0,
      .start_of_line = 1,
      .current_col = 0,
      .last_line_blank = true,
      .block_start = true,
      .comments = module->comments,
      .comment_count = module->comment_count,
      .next_comment_idx = 0,
      .source = source,
      .verbatim_count = 0,
  };
  collect_verbatim_ranges(&pp, module);

  size_t last_verbatim_end = 0;
  bool wrote_header = false;

  if (module->export_to_main) {
    pp_write(&pp, "export");
    pp_newline(&pp);
    wrote_header = true;
  }

  const AstImport* imp = module->imports;
  while (imp) {
    VerbatimRange* r = pp_find_verbatim_range(&pp, imp->line);
    if (r) {
      if (r->end_line > last_verbatim_end) {
        pp_print_verbatim_range(&pp, r->start_line, r->end_line);
        last_verbatim_end = r->end_line;
      }
      while (imp && imp->line <= r->end_line) imp = imp->next;
      continue;
    }
    pp_print_import(&pp, imp);
    pp.block_start = false;
    wrote_header = true;
    imp = imp->next;
  }

  for (const AstCHeader* hdr = module->c_headers; hdr; hdr = hdr->next) {
    pp_write(&pp, "cheader ");
    pp_write_string_literal(&pp, hdr->path);
    pp_newline(&pp);
    wrote_header = true;
  }

  /* One blank line between top-level declarations (and after the header). */
  const AstDecl* decl = module->decls;
  bool first = !wrote_header;
  while (decl) {
    VerbatimRange* r = pp_find_verbatim_range(&pp, decl->line);
    if (r) {
      if (r->end_line > last_verbatim_end) {
        if (!first) pp_blank_line(&pp);
        pp_print_verbatim_range(&pp, r->start_line, r->end_line);
        last_verbatim_end = r->end_line;
        first = false;
      }
      while (decl && decl->line <= r->end_line) decl = decl->next;
      continue;
    }
    pp.block_start = false;
    if (!first) {
      /* Leading comments keep their own gap to the declaration; the blank
       * line goes before whichever comes first. */
      pp_blank_line(&pp);
    }
    pp_print_decl(&pp, decl);
    first = false;
    decl = decl->next;
  }

  pp.block_start = false;
  pp_flush_comments_before(&pp, (size_t)-1);
  if (!pp.start_of_line) pp_newline(&pp);
}
