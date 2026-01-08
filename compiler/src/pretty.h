/* pretty.h - Rae pretty printer */

#ifndef PRETTY_H
#define PRETTY_H

#include <stdio.h>
#include "ast.h"

void pretty_print_module(const AstModule* module, const char* source, FILE* out);

#endif /* PRETTY_H */
