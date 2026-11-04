#ifndef RAE_PACK_EDIT_H
#define RAE_PACK_EDIT_H

#include <stdbool.h>
#include <stddef.h>

/* Editing a .raepack's text: the pack is the one file that holds a project's pins
 * (docs/toolchain-and-packages-decision.md), and `rae add`, `fetch` and `update`
 * write it. A build never does. */

/* `text` with the top-level block `key: { ... }` replaced by `block`, which holds
 * whole lines with their indentation. A pack with no such block gets it before the
 * first of `before_keys` (NULL-terminated) that it has, else before its closing
 * brace. A malloc'd buffer; NULL if the text has no pack block to edit. */
char* rae_pack_set_block(const char* text, const char* key, const char* block,
                         const char* const* before_keys);

/* Whether the pack has the top-level block `key: { ... }`. */
bool rae_pack_has_block(const char* text, const char* key);

/* The pack's `rae` block: its version, commit and check ("" for each it has none of).
 * False when it has no `rae` block. */
bool rae_pack_read_pin(const char* text, char* version, size_t version_len, char* commit,
                       size_t commit_len, char* check, size_t check_len);

#endif /* RAE_PACK_EDIT_H */
