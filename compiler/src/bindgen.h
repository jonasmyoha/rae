#ifndef RAE_BINDGEN_H
#define RAE_BINDGEN_H

// C-header -> Rae FFI binding generator (general FFI, #498). An elemental part
// of the compiler: `rae bindgen <header.h> [more.h ...] --out <file.rae>
// [--cheader <include>] [--module-comment <text>]`. Parses the C headers and
// emits deterministic, reproducible low-level Rae bindings (c_struct types,
// enum/flag/constant values, and extern("symbol") functions) that bind
// directly to the C ABI with no shim. First target: WebGPU (webgpu.h+wgpu.h).
int bindgen_run(int argc, char** argv);

#endif
