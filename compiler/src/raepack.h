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
  /* `exports: "add, greet"` on the wasm target: build a WASM module (a WASI
   * reactor) exporting these functions by name (docs/wasm-modules.md);
   * empty for a program */
  Str exports;
  /* `requests: "log, clock"` on the wasm target: the capabilities the module
   * asks a host for (docs/wasm-interfaces.md §4). A request never grants; the
   * module build fails on an import no request (nor `runtime`) covers. */
  Str requests;
  /* `imports: "game.describe"` on the wasm target: `extern` functions that
   * become WASM imports of the module `rae`, each in the named capability
   * (docs/wasm-interfaces.md §6) */
  Str imports;
  RaePackSource* sources;
  size_t source_count;
  struct RaePackTarget* next;
} RaePackTarget;

typedef struct RaePackField RaePackField;
typedef struct RaePackBlock RaePackBlock;

/* One `dep <name>: { path: "..." }` or `dep <name>: { git: "...", rev: "..." }`
 * entry of the pack's `dependencies` block (#933). Exactly one of path/git is
 * set; rev is optional and only meaningful with git. */
typedef struct RaePackDep {
  Str name;
  Str path;
  Str git;
  /* `rev` is no longer a pin: it is read only to answer it with the repair */
  Str rev;
  /* The exact version; with a git dep required, and it names the tag. */
  Str version;
  /* git only: one exact commit (optional), the package's folder in the repo
   * (optional), and the tool-written hash of its files */
  Str commit;
  Str subdir;
  Str hash;
  struct RaePackDep* next;
} RaePackDep;

typedef struct {
  Str name;
  Str format;
  int64_t version;
  Str default_target;
  /* Optional toolchain requirement: the string from `rae: { version: "..." }`
   * (#931). Empty (len 0) when the pack declares no requirement. */
  Str rae_version;
  /* The optional exact commit of the same block. */
  Str rae_commit;
  /* The folders of a package's files read at run time, space separated: `assets: "assets maps"` */
  Str assets;
  /* A package's C: the `native` block (rae_package_native.h). All empty for none. */
  Str native_build;
  Str native_header;
  Str native_include;
  Str native_link;
  Str native_link_browser;
  Str native_link_linux;
  Str native_sources_device;
  Str native_defines_device;
  /* Android's device build, when it differs (an Apple .m source): else the device ones */
  Str native_sources_android;
  Str native_defines_android;
  /* Optional `meta: { name: "...", version: "..." }`: what the program is
   * called and its release (the App Store's version); the program reads
   * them as Pack.name / Pack.version. Empty (len 0) when absent. */
  Str meta_name;
  Str meta_version;
  /* Optional `dependencies` block (#933), in declaration order. */
  RaePackDep* deps;
  size_t dep_count;
  RaePackTarget* targets;
  size_t target_count;
  RaePackBlock* raw;
  Arena* arena;
} RaePack;

bool raepack_parse_file(const char* file_path, RaePack* out, bool strict);
/* A dependency's own pack (#64888955): format and version required, targets
 * optional (a library is built by the project that depends on it). */
bool raepack_parse_library_file(const char* file_path, RaePack* out);
void raepack_pretty_print(const RaePack* pack, FILE* out);
void raepack_free(RaePack* pack);
const RaePackTarget* raepack_find_target(const RaePack* pack, Str id);
const char* raepack_emit_name(RaePackEmit emit);

#endif /* RAEPACK_H */
