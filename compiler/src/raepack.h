#ifndef RAEPACK_H
#define RAEPACK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "arena.h"
#include "str.h"

typedef enum {
  RAEPACK_EMIT_LIVE = 0,
  RAEPACK_EMIT_COMPILED,
  RAEPACK_EMIT_HYBRID
} RaePackEmit;

typedef struct RaePackSource {
  Str path;
  RaePackEmit emit;
  struct RaePackSource* next;
} RaePackSource;

typedef struct RaePackTarget {
  Str id;
  Str label;
  Str entry;
  RaePackSource* sources;
  size_t source_count;
  struct RaePackTarget* next;
} RaePackTarget;

typedef struct RaePackField RaePackField;
typedef struct RaePackBlock RaePackBlock;

typedef struct {
  Str name;
  Str format;
  int64_t version;
  Str default_target;
  RaePackTarget* targets;
  size_t target_count;
  RaePackBlock* raw;
  Arena* arena;
} RaePack;

bool raepack_parse_file(const char* file_path, RaePack* out_pack);
void raepack_pretty_print(const RaePack* pack, FILE* out);
void raepack_free(RaePack* pack);
const RaePackTarget* raepack_find_target(const RaePack* pack, Str id);
const char* raepack_emit_name(RaePackEmit emit);

#endif /* RAEPACK_H */
