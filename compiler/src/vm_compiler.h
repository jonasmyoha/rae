#ifndef VM_COMPILER_H
#define VM_COMPILER_H

#include <stdbool.h>

#include "ast.h"
#include "vm_chunk.h"

bool vm_compile_module(const AstModule* module, Chunk* chunk, const char* file_path);

#endif /* VM_COMPILER_H */
