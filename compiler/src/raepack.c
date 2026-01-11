#include "raepack.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "diag.h"
#include "lexer.h"

typedef enum {
  RAEPACK_VALUE_STRING,
  RAEPACK_VALUE_INT,
  RAEPACK_VALUE_IDENT,
  RAEPACK_VALUE_BLOCK
} RaePackValueKind;

typedef struct RaePackValue {
  RaePackValueKind kind;
  union {
    Str string;
    int64_t integer;
    Str ident;
    struct RaePackBlock* block;
  } as;
} RaePackValue;

struct RaePackField {
  Str key;
  Str tag;
  RaePackValue value;
  size_t line;
  size_t column;
  RaePackField* next;
};

struct RaePackBlock {
  RaePackField* fields;
};

typedef struct {
  const Token* tokens;
  size_t count;
  size_t index;
  const char* file_path;
  Arena* arena;
  bool had_error;
} RaePackParser;

static const Token* parser_peek(const RaePackParser* parser) {
  if (!parser || parser->index >= parser->count) return NULL;
  return &parser->tokens[parser->index];
}

static const Token* parser_peek_at(const RaePackParser* parser, size_t offset) {
  if (!parser) return NULL;
  size_t idx = parser->index + offset;
  if (idx >= parser->count) return NULL;
  return &parser->tokens[idx];
}

static const Token* parser_advance(RaePackParser* parser) {
  if (!parser || parser->index >= parser->count) return NULL;
  return &parser->tokens[parser->index++];
}

static void parser_error(RaePackParser* parser, const Token* token, const char* message) {
  if (!parser) return;
  if (!token) {
    diag_error(parser->file_path, 0, 0, message);
  } else {
    diag_error(parser->file_path, (int)token->line, (int)token->column, message);
  }
  parser->had_error = true;
}

static bool token_is_ident_like(const Token* token) {
  if (!token) return false;
  if (token->kind == TOK_IDENT) return true;
  return token->kind >= TOK_KW_TYPE && token->kind <= TOK_KW_PRIV;
}

static Str parser_copy_str(RaePackParser* parser, Str value) {
  if (!parser || value.len == 0) return (Str){.data = "", .len = 0};
  char* buffer = arena_alloc(parser->arena, value.len + 1);
  if (!buffer) return (Str){.data = "", .len = 0};
  memcpy(buffer, value.data, value.len);
  buffer[value.len] = '\0';
  return (Str){.data = buffer, .len = value.len};
}

static Str parse_string_literal_value(RaePackParser* parser, const Token* token) {
  if (!token || token->kind != TOK_STRING || token->lexeme.len < 2) {
    parser_error(parser, token, "invalid string literal in raepack");
    return (Str){.data = "", .len = 0};
  }
  size_t capacity = token->lexeme.len > 1 ? token->lexeme.len - 1 : 1;
  char* buffer = arena_alloc(parser->arena, capacity + 1);
  if (!buffer) {
    parser_error(parser, token, "out of memory while parsing raepack string");
    return (Str){.data = "", .len = 0};
  }
  size_t out_len = 0;
  const char* data = token->lexeme.data;
  size_t len = token->lexeme.len;
  for (size_t i = 1; i + 1 < len; ++i) {
    char c = data[i];
    if (c == '\\' && i + 1 < len - 1) {
      i += 1;
      char esc = data[i];
      switch (esc) {
        case 'n':
          c = '\n';
          break;
        case 't':
          c = '\t';
          break;
        case '\\':
          c = '\\';
          break;
        case '"':
          c = '"';
          break;
        default:
          c = esc;
          break;
      }
    }
    buffer[out_len++] = c;
  }
  buffer[out_len] = '\0';
  return (Str){.data = buffer, .len = out_len};
}

static bool parse_integer_value(const Token* token, int64_t* out) {
  if (!token || token->kind != TOK_INTEGER || !out) return false;

  const char* p = token->lexeme.data;
  const char* end = p + token->lexeme.len;
  long long value = 0;

  if (p == end) {
    return false; // Empty lexeme should not happen for TOK_INTEGER
  }

  while (p < end) {
    if (*p >= '0' && *p <= '9') {
      value = value * 10 + (*p - '0');
    } else {
      // This case should be impossible if the lexer is correct
      return false;
    }
    p++;
  }

  *out = value;
  return true;
}

static RaePackBlock* parse_block(RaePackParser* parser);

static RaePackValue parse_value(RaePackParser* parser) {
  RaePackValue value = {.kind = RAEPACK_VALUE_IDENT, .as.ident = str_from_cstr("")};
  const Token* token = parser_peek(parser);
  if (!token) {
    parser_error(parser, token, "unexpected end of raepack value");
    return value;
  }
  if (token->kind == TOK_LBRACE) {
    RaePackBlock* block = parse_block(parser);
    value.kind = RAEPACK_VALUE_BLOCK;
    value.as.block = block;
    return value;
  }
  if (token->kind == TOK_STRING) {
    parser_advance(parser);
    value.kind = RAEPACK_VALUE_STRING;
    value.as.string = parse_string_literal_value(parser, token);
    return value;
  }
  if (token->kind == TOK_INTEGER) {
    parser_advance(parser);
    value.kind = RAEPACK_VALUE_INT;
    if (!parse_integer_value(token, &value.as.integer)) {
      parser_error(parser, token, "invalid integer literal in raepack");
    }
    return value;
  }
  if (token_is_ident_like(token)) {
    parser_advance(parser);
    value.kind = RAEPACK_VALUE_IDENT;
    value.as.ident = parser_copy_str(parser, token->lexeme);
    return value;
  }
  if (token->kind == TOK_LPAREN || token->kind == TOK_RPAREN) {
    parser_error(parser, token, "raepack values must use '{ }' blocks, not '( )'");
    parser_advance(parser);
    return value;
  }
  parser_error(parser, token, "unexpected token in raepack value");
  parser_advance(parser);
  return value;
}

static RaePackField* parse_field(RaePackParser* parser) {
  const Token* key_token = parser_peek(parser);
  if (!token_is_ident_like(key_token)) {
    parser_error(parser, key_token, "expected field name in raepack");
    return NULL;
  }
  parser_advance(parser);
  Str key = parser_copy_str(parser, key_token->lexeme);
  Str tag = str_from_cstr("");

  if (str_eq_cstr(key, "target")) {
    const Token* tag_token = parser_peek(parser);
    const Token* colon = parser_peek_at(parser, 1);
    if (token_is_ident_like(tag_token) && colon && colon->kind == TOK_COLON) {
      parser_advance(parser);
      tag = parser_copy_str(parser, tag_token->lexeme);
    }
  }

  const Token* colon = parser_peek(parser);
  if (!colon || colon->kind != TOK_COLON) {
    parser_error(parser, colon, "expected ':' after raepack field");
    return NULL;
  }
  parser_advance(parser);

  RaePackValue value = parse_value(parser);
  RaePackField* field = arena_alloc(parser->arena, sizeof(RaePackField));
  if (!field) return NULL;
  field->key = key;
  field->tag = tag;
  field->value = value;
  field->line = key_token->line;
  field->column = key_token->column;
  field->next = NULL;
  return field;
}

static RaePackBlock* parse_block(RaePackParser* parser) {
  const Token* open = parser_peek(parser);
  if (!open || open->kind != TOK_LBRACE) {
    parser_error(parser, open, "expected '{' to start raepack block");
    return NULL;
  }
  parser_advance(parser);
  RaePackBlock* block = arena_alloc(parser->arena, sizeof(RaePackBlock));
  if (!block) return NULL;
  block->fields = NULL;
  RaePackField* tail = NULL;
  while (!parser->had_error) {
    const Token* token = parser_peek(parser);
    if (!token) break;
    if (token->kind == TOK_COMMA) {
      parser_advance(parser);
      continue;
    }
    if (token->kind == TOK_RBRACE) {
      parser_advance(parser);
      break;
    }
    if (token->kind == TOK_EOF) {
      parser_error(parser, token, "unexpected end of raepack block");
      break;
    }
    RaePackField* field = parse_field(parser);
    if (!field) break;
    if (!block->fields) {
      block->fields = field;
    } else {
      tail->next = field;
    }
    tail = field;
  }
  return block;
}

static bool parse_pack_header(RaePackParser* parser, RaePack* pack) {
  const Token* pack_token = parser_peek(parser);
  if (!pack_token || pack_token->kind != TOK_KW_PACK) {
    parser_error(parser, pack_token, "raepack must start with 'pack <Name>'");
    return false;
  }
  parser_advance(parser);

  const Token* name_token = parser_peek(parser);
  if (!token_is_ident_like(name_token)) {
    parser_error(parser, name_token, "expected pack name after 'pack'");
    return false;
  }
  parser_advance(parser);
  pack->name = parser_copy_str(parser, name_token->lexeme);

  // Optionally consume a colon
  const Token* colon = parser_peek(parser);
  if (colon && colon->kind == TOK_COLON) {
    parser_advance(parser);
  }

  RaePackBlock* block = parse_block(parser);
  pack->raw = block;
  return block != NULL;
}

static RaePackEmit emit_from_ident(RaePackParser* parser, const RaePackField* field, Str ident,
                                   bool* ok) {
  if (str_eq_cstr(ident, "live")) {
    return RAEPACK_EMIT_LIVE;
  }
  if (str_eq_cstr(ident, "compiled")) {
    return RAEPACK_EMIT_COMPILED;
  }
  if (str_eq_cstr(ident, "hybrid")) {
    return RAEPACK_EMIT_HYBRID;
  }
  if (ok) *ok = false;
  if (field) {
    diag_error(parser->file_path, (int)field->line, (int)field->column,
               "emit must be one of: live | compiled | hybrid");
  } else {
    diag_error(parser->file_path, 0, 0, "emit must be one of: live | compiled | hybrid");
  }
  return RAEPACK_EMIT_LIVE;
}

static char* join_path(const char* base, const char* rel) {
  size_t base_len = strlen(base);
  size_t rel_len = strlen(rel);
  size_t total = base_len + rel_len + 2;
  char* result = malloc(total);
  if (!result) return NULL;
  if (base_len == 0) {
    snprintf(result, total, "%s", rel);
    return result;
  }
  if (rel_len == 0) {
    snprintf(result, total, "%s", base);
    return result;
  }
  if (base[base_len - 1] == '/' || base[base_len - 1] == '\\') {
    snprintf(result, total, "%s%s", base, rel);
  } else {
    snprintf(result, total, "%s/%s", base, rel);
  }
  return result;
}

static bool path_is_absolute(const char* path) {
  if (!path || path[0] == '\0') return false;
  if (path[0] == '/' || path[0] == '\\') return true;
  if (isalpha((unsigned char)path[0]) && path[1] == ':') return true;
  return false;
}

static char* resolve_path(const char* base_dir, Str rel) {
  char* rel_cstr = str_to_cstr(rel);
  if (!rel_cstr) return NULL;
  char* merged = NULL;
  if (path_is_absolute(rel_cstr)) {
    merged = strdup(rel_cstr);
  } else {
    merged = join_path(base_dir, rel_cstr);
  }
  free(rel_cstr);
  if (!merged) return NULL;
  char* resolved = realpath(merged, NULL);
  free(merged);
  return resolved;
}

static bool path_has_prefix(const char* path, const char* prefix) {
  size_t len = strlen(prefix);
  if (strncmp(path, prefix, len) != 0) return false;
  if (path[len] == '\0') return true;
  return path[len] == '/' || path[len] == '\\';
}

static bool validate_entry_in_sources(RaePackParser* parser,
                                      const char* base_dir,
                                      const RaePackTarget* target,
                                      const RaePackField* entry_field) {
  if (!target) return false;
  char* entry_path = resolve_path(base_dir, target->entry);
  if (!entry_path) {
    diag_error(parser->file_path, (int)entry_field->line, (int)entry_field->column,
               "entry path could not be resolved");
    return false;
  }
  struct stat entry_stat;
  if (stat(entry_path, &entry_stat) != 0 || !S_ISREG(entry_stat.st_mode)) {
    diag_error(parser->file_path, (int)entry_field->line, (int)entry_field->column,
               "entry must be a file included by sources");
    free(entry_path);
    return false;
  }

  bool matched = false;
  for (const RaePackSource* source = target->sources; source; source = source->next) {
    char* source_path = resolve_path(base_dir, source->path);
    if (!source_path) {
      continue;
    }
    struct stat source_stat;
    if (stat(source_path, &source_stat) != 0) {
      free(source_path);
      continue;
    }
    if (S_ISDIR(source_stat.st_mode)) {
      if (path_has_prefix(entry_path, source_path)) {
        matched = true;
      }
    } else if (S_ISREG(source_stat.st_mode)) {
      if (strcmp(entry_path, source_path) == 0) {
        matched = true;
      }
    }
    free(source_path);
    if (matched) break;
  }

  if (!matched) {
    diag_error(parser->file_path, (int)entry_field->line, (int)entry_field->column,
               "entry must be included by sources");
  }
  free(entry_path);
  return matched;
}

static bool parse_sources(RaePackParser* parser,
                          RaePackTarget* target,
                          RaePackBlock* block,
                          const RaePackField* field) {
  if (!block) return false;
  RaePackSource* tail = NULL;
  for (const RaePackField* entry = block->fields; entry; entry = entry->next) {
    if (!str_eq_cstr(entry->key, "source")) {
      continue;
    }
    if (entry->value.kind != RAEPACK_VALUE_BLOCK) {
      diag_error(parser->file_path, (int)entry->line, (int)entry->column,
                 "source must be a block");
      parser->had_error = true;
      return false;
    }
    RaePackBlock* source_block = entry->value.as.block;
    Str path_value = str_from_cstr("");
    Str emit_ident = str_from_cstr("");
    bool saw_path = false;
    bool saw_emit = false;
    for (const RaePackField* source_field = source_block->fields;
         source_field;
         source_field = source_field->next) {
      if (str_eq_cstr(source_field->key, "path")) {
        if (source_field->value.kind != RAEPACK_VALUE_STRING) {
          diag_error(parser->file_path, (int)source_field->line, (int)source_field->column,
                     "source path must be a string");
          parser->had_error = true;
          return false;
        }
        path_value = source_field->value.as.string;
        saw_path = true;
      } else if (str_eq_cstr(source_field->key, "emit")) {
        if (source_field->value.kind != RAEPACK_VALUE_IDENT) {
          diag_error(parser->file_path, (int)source_field->line, (int)source_field->column,
                     "emit must be an identifier");
          parser->had_error = true;
          return false;
        }
        emit_ident = source_field->value.as.ident;
        saw_emit = true;
      }
    }
    if (!saw_path || !saw_emit) {
      diag_error(parser->file_path, (int)entry->line, (int)entry->column,
                 "source requires path and emit");
      parser->had_error = true;
      return false;
    }
    bool ok = true;
    RaePackEmit emit = emit_from_ident(parser, entry, emit_ident, &ok);
    if (!ok) {
      parser->had_error = true;
      return false;
    }
    RaePackSource* source = arena_alloc(parser->arena, sizeof(RaePackSource));
    if (!source) return false;
    source->path = path_value;
    source->emit = emit;
    source->next = NULL;
    if (!target->sources) {
      target->sources = source;
    } else {
      tail->next = source;
    }
    tail = source;
    target->source_count += 1;
  }
  if (target->source_count == 0) {
    diag_error(parser->file_path, (int)field->line, (int)field->column,
               "sources must include at least one source entry");
    parser->had_error = true;
    return false;
  }
  return true;
}

static bool parse_target_block(RaePackParser* parser,
                               RaePackTarget* target,
                               RaePackBlock* block) {
  bool saw_label = false;
  bool saw_entry = false;
  bool saw_sources = false;
  const RaePackField* entry_field = NULL;
  for (const RaePackField* field = block->fields; field; field = field->next) {
    if (str_eq_cstr(field->key, "label")) {
      if (field->value.kind != RAEPACK_VALUE_STRING) {
        diag_error(parser->file_path, (int)field->line, (int)field->column,
                   "label must be a string");
        parser->had_error = true;
        return false;
      }
      target->label = field->value.as.string;
      saw_label = true;
    } else if (str_eq_cstr(field->key, "entry")) {
      if (field->value.kind != RAEPACK_VALUE_STRING) {
        diag_error(parser->file_path, (int)field->line, (int)field->column,
                   "entry must be a string");
        parser->had_error = true;
        return false;
      }
      target->entry = field->value.as.string;
      entry_field = field;
      saw_entry = true;
    } else if (str_eq_cstr(field->key, "sources")) {
      if (field->value.kind != RAEPACK_VALUE_BLOCK) {
        diag_error(parser->file_path, (int)field->line, (int)field->column,
                   "sources must be a block");
        parser->had_error = true;
        return false;
      }
      if (!parse_sources(parser, target, field->value.as.block, field)) {
        return false;
      }
      saw_sources = true;
    }
  }
  if (!saw_label || !saw_entry || !saw_sources) {
    diag_error(parser->file_path, 0, 0, "target requires label, entry, and sources");
    parser->had_error = true;
    return false;
  }
  if (entry_field) {
    const char* base_dir = parser->file_path;
    char* dir_copy = strdup(base_dir);
    if (!dir_copy) return false;
    char* slash = strrchr(dir_copy, '/');
    if (slash) {
      *slash = '\0';
    }
    char* resolved_dir = realpath(dir_copy, NULL);
    free(dir_copy);
    if (!resolved_dir) {
      diag_error(parser->file_path, (int)entry_field->line, (int)entry_field->column,
                 "raepack directory could not be resolved");
      parser->had_error = true;
      return false;
    }
    bool ok = validate_entry_in_sources(parser, resolved_dir, target, entry_field);
    free(resolved_dir);
    return ok;
  }
  return true;
}

static bool parse_targets(RaePackParser* parser, RaePack* pack, RaePackBlock* block) {
  RaePackTarget* tail = NULL;
  for (const RaePackField* field = block->fields; field; field = field->next) {
    if (!str_eq_cstr(field->key, "target")) {
      continue;
    }
    if (str_is_empty(field->tag)) {
      diag_error(parser->file_path, (int)field->line, (int)field->column,
                 "target entries must use 'target <id>:'");
      parser->had_error = true;
      return false;
    }
    if (field->value.kind != RAEPACK_VALUE_BLOCK) {
      diag_error(parser->file_path, (int)field->line, (int)field->column,
                 "target must be a block");
      parser->had_error = true;
      return false;
    }
    RaePackTarget* target = arena_alloc(parser->arena, sizeof(RaePackTarget));
    if (!target) return false;
    memset(target, 0, sizeof(RaePackTarget));
    target->id = field->tag;
    if (!parse_target_block(parser, target, field->value.as.block)) {
      return false;
    }
    if (!pack->targets) {
      pack->targets = target;
    } else {
      tail->next = target;
    }
    tail = target;
    pack->target_count += 1;
  }
  if (pack->target_count == 0) {
    diag_error(parser->file_path, 0, 0, "targets must include at least one target");
    parser->had_error = true;
    return false;
  }
  return true;
}

static bool parse_required_fields(RaePackParser* parser, RaePack* pack) {
  bool saw_format = false;
  bool saw_version = false;
  bool saw_default = false;
  bool saw_targets = false;
  for (const RaePackField* field = pack->raw ? pack->raw->fields : NULL;
       field;
       field = field->next) {
    if (str_eq_cstr(field->key, "format")) {
      if (saw_format) {
        diag_error(parser->file_path, (int)field->line, (int)field->column,
                   "duplicate format field in raepack");
        parser->had_error = true;
        return false;
      }
      if (field->value.kind != RAEPACK_VALUE_STRING) {
        diag_error(parser->file_path, (int)field->line, (int)field->column,
                   "format must be a string");
        parser->had_error = true;
        return false;
      }
      pack->format = field->value.as.string;
      saw_format = true;
    } else if (str_eq_cstr(field->key, "version")) {
      if (saw_version) {
        diag_error(parser->file_path, (int)field->line, (int)field->column,
                   "duplicate version field in raepack");
        parser->had_error = true;
        return false;
      }
      if (field->value.kind != RAEPACK_VALUE_INT) {
        diag_error(parser->file_path, (int)field->line, (int)field->column,
                   "version must be an integer");
        parser->had_error = true;
        return false;
      }
      pack->version = field->value.as.integer;
      saw_version = true;
    } else if (str_eq_cstr(field->key, "defaultTarget")) {
      if (saw_default) {
        diag_error(parser->file_path, (int)field->line, (int)field->column,
                   "duplicate defaultTarget field in raepack");
        parser->had_error = true;
        return false;
      }
      if (field->value.kind != RAEPACK_VALUE_IDENT) {
        diag_error(parser->file_path, (int)field->line, (int)field->column,
                   "defaultTarget must be an identifier");
        parser->had_error = true;
        return false;
      }
      pack->default_target = field->value.as.ident;
      saw_default = true;
    } else if (str_eq_cstr(field->key, "targets")) {
      if (saw_targets) {
        diag_error(parser->file_path, (int)field->line, (int)field->column,
                   "duplicate targets field in raepack");
        parser->had_error = true;
        return false;
      }
      if (field->value.kind != RAEPACK_VALUE_BLOCK) {
        diag_error(parser->file_path, (int)field->line, (int)field->column,
                   "targets must be a block");
        parser->had_error = true;
        return false;
      }
      if (!parse_targets(parser, pack, field->value.as.block)) {
        return false;
      }
      saw_targets = true;
    }
  }

  if (!saw_format || !saw_version || !saw_default || !saw_targets) {
    diag_error(parser->file_path, 0, 0,
               "raepack requires format, version, defaultTarget, and targets");
    parser->had_error = true;
    return false;
  }
  if (!str_eq_cstr(pack->format, "raepack")) {
    diag_error(parser->file_path, 0, 0, "format must be \"raepack\"");
    parser->had_error = true;
    return false;
  }
  if (pack->version <= 0) {
    diag_error(parser->file_path, 0, 0, "version must be a positive integer");
    parser->had_error = true;
    return false;
  }
  return true;
}

bool raepack_parse_file(const char* file_path, RaePack* out, bool strict) {
  if (out) memset(out, 0, sizeof(RaePack));
  size_t file_size = 0;
  char* source = read_file(file_path, &file_size);
  if (!source) {
    fprintf(stderr, "error: could not read raepack '%s'\n", file_path);
    return false;
  }
  Arena* arena = arena_create(64 * 1024);
  if (!arena) {
    free(source);
    diag_fatal("could not allocate arena");
  }
  TokenList tokens = lexer_tokenize(arena, file_path, source, file_size, strict);
  if (tokens.had_error) {
      free(source);
      arena_destroy(arena);
      memset(out, 0, sizeof(RaePack));
      return false;
  }
  RaePackParser parser = {
    .tokens = tokens.data,
    .count = tokens.count,
    .index = 0,
    .file_path = file_path,
    .arena = arena,
    .had_error = false
  };
  if (!parser.had_error) {
    out->arena = arena;
    if (!parse_pack_header(&parser, out)) {
      parser.had_error = true;
    } else if (!parse_required_fields(&parser, out)) {
      parser.had_error = true;
    } else {
      const Token* trailing = parser_peek(&parser);
      if (trailing && trailing->kind != TOK_EOF) {
        parser_error(&parser, trailing, "unexpected tokens after raepack");
      }
    }
  }
  free(source);
  if (parser.had_error) {
    arena_destroy(arena);
    memset(out, 0, sizeof(RaePack));
    return false;
  }
  out->arena = arena;
  return true;
}

static void pp_indent(FILE* out, int level) {
  for (int i = 0; i < level; i++) fputs("  ", out);
}

static void pp_value(const RaePackValue* value, FILE* out, int indent);

static void pp_block(const RaePackBlock* block, FILE* out, int indent) {
  fprintf(out, "{\n");
  for (const RaePackField* field = block->fields; field; field = field->next) {
    pp_indent(out, indent + 1);
    fprintf(out, "%.*s", (int)field->key.len, field->key.data);
    if (field->tag.len > 0) {
      fprintf(out, " %.*s", (int)field->tag.len, field->tag.data);
    }
    fprintf(out, ": ");
    pp_value(&field->value, out, indent + 1);
    fprintf(out, "\n");
  }
  pp_indent(out, indent);
  fprintf(out, "}");
}

static void pp_value(const RaePackValue* value, FILE* out, int indent) {
  switch (value->kind) {
    case RAEPACK_VALUE_STRING:
      fprintf(out, "\"%.*s\"", (int)value->as.string.len, value->as.string.data);
      break;
    case RAEPACK_VALUE_INT:
      fprintf(out, "%lld", (long long)value->as.integer);
      break;
    case RAEPACK_VALUE_IDENT:
      fprintf(out, "%.*s", (int)value->as.ident.len, value->as.ident.data);
      break;
    case RAEPACK_VALUE_BLOCK:
      pp_block(value->as.block, out, indent);
      break;
  }
}

void raepack_pretty_print(const RaePack* pack, FILE* out) {
  if (!pack) return;
  fprintf(out, "pack %.*s ", (int)pack->name.len, pack->name.data);
  pp_block(pack->raw, out, 0);
  fprintf(out, "\n");
}

void raepack_free(RaePack* pack) {
  if (!pack) return;
  if (pack->arena) {
    arena_destroy(pack->arena);
  }
  memset(pack, 0, sizeof(RaePack));
}

const RaePackTarget* raepack_find_target(const RaePack* pack, Str id) {
  if (!pack) return NULL;
  for (const RaePackTarget* target = pack->targets; target; target = target->next) {
    if (str_eq(target->id, id)) {
      return target;
    }
  }
  return NULL;
}

const char* raepack_emit_name(RaePackEmit emit) {
  switch (emit) {
    case RAEPACK_EMIT_LIVE:
      return "live";
    case RAEPACK_EMIT_COMPILED:
      return "compiled";
    case RAEPACK_EMIT_HYBRID:
      return "hybrid";
  }
  return "unknown";
}
