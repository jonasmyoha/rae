#ifndef C_BACKEND_H
#define C_BACKEND_H

#include <stdbool.h>

#include "ast.h"

struct VmRegistry;

bool c_backend_emit_module(const AstModule* module, const char* out_path, struct VmRegistry* registry);

#endif /* C_BACKEND_H */
