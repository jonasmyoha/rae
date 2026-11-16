/* import_case — an import's spelling checked against the names on disk
 * (docs/module-model-options.md rule 8). */
#ifndef RAE_IMPORT_CASE_H
#define RAE_IMPORT_CASE_H
#include <stdbool.h>
#include <stddef.h>

/* Walk the import path `normalized` through the places an import resolves from, in the same
 * order (a dependency's folder for its prefix, the project root, its lib/, ../lib and lib from
 * the working folder, the compiler's stdlib), matching each directory entry while ignoring case.
 * The first place with a match decides. When its spelling on disk differs from the import, writes
 * the diagnostic into `message` and answers false; true otherwise (also when nothing matches: the
 * resolver reports a missing module itself). `keyword` is "import" or "open". */
bool import_case_check(const char* normalized,
                       const char* keyword,
                       const char* root,
                       const char* stdlib,
                       char* message,
                       size_t cap);

#endif
