#ifndef C_BACKEND_H
#define C_BACKEND_H

#include <stdbool.h>

#include "ast.h"

bool c_backend_emit(const AstModule* module, const char* out_path);

#endif /* C_BACKEND_H */
