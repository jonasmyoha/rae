/* Hosek-Wilkie coefficient dataset, folded into the runtime translation unit.
 *
 * Included here rather than added to a link line because that is how this
 * runtime carries third-party C (see lodepng in runtime_image_sdl3.c): the
 * generated-program link command, the compiler Makefile, the devtools config
 * and the wasm build all pull in rae_runtime.c, so one #include reaches every
 * build path instead of four lists needing to agree.
 *
 * Provenance and licence: third_party/hosek_wilkie/README.md. The dataset is
 * the authors' published data under a 3-clause BSD licence; the notice travels
 * inside the data header itself.
 */
#include "../../third_party/hosek_wilkie/rae_hosek.c"
