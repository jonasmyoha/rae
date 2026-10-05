#ifndef BINDGEN_PREPROCESS_H
#define BINDGEN_PREPROCESS_H

#include <stddef.h>

// Take macro `name` (len bytes) as defined when evaluating #if.
void bindgen_add_defined(const char* name, size_t len);

// Blank out (keeping newlines) every line of `s` in an inactive branch of
// #if/#ifdef/#ifndef/#elif/#else/#endif.
void bindgen_preprocess_conditionals(char* s);

#endif
