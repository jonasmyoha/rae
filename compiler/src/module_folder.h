/* module_folder — a module is a PascalCase file or a PascalCase folder
 * (docs/module-model-options.md rules 1-5 and 9). */
#ifndef RAE_MODULE_FOLDER_H
#define RAE_MODULE_FOLDER_H
#include <stdbool.h>
#include <stddef.h>

/* `path` is a folder (want_dir) or a file whose last name is spelled exactly as written (macOS and
 * Windows ignore case in stat) */
bool module_folder_exists_exact(const char* path, bool want_dir);

/* True when a file or folder name is PascalCase (starts with A-Z): a module */
bool module_folder_name_is_module(const char* name, size_t length);

/* Where `import <normalized>` resolves, as a module folder: 1 with the folder in `dir_out` when
 * the first place that has the name (a dependency's folder after its prefix, the project, its
 * lib/, ../lib, lib, the stdlib) holds a PascalCase folder by it; 0 when it holds a file, or
 * nothing; -1 when it holds both a file and a folder of the name (`error` names both). */
int module_folder_resolve(const char* normalized,
                          const char* root,
                          const char* stdlib,
                          char* dir_out,
                          size_t dir_cap,
                          char* error,
                          size_t error_cap);

/* One file of a module folder: its path and its path inside the folder without `.rae`
 * (`platform/Web`) */
typedef struct {
  char path[1024];
  char stem[512];
} ModuleFolderFile;

typedef struct {
  ModuleFolderFile* items;
  int count;
  int capacity;
} ModuleFolderFiles;

/* Every .rae file of the module folder `dir`, sorted, with its camelCase subfolders' files; a
 * PascalCase subfolder is a module of its own and is skipped. False with `error` set when a file
 * and a folder of one name sit in one place inside it. */
bool module_folder_list(const char* dir, ModuleFolderFiles* out, char* error, size_t error_cap);
void module_folder_files_free(ModuleFolderFiles* files);

/* The module a file import reaches into: when a folder segment of `normalized` (not the last) is
 * PascalCase, its path up to the deepest such folder goes to `module_out` and the answer is true
 * (`import Gpu2d/Images` for the file Gpu2d/Images.rae is the module Gpu2d). */
bool module_folder_owner(const char* normalized, char* module_out, size_t cap);

#endif
