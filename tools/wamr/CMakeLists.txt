# librae_wamr.a: WAMR's fast interpreter (no AOT, no JIT, no libc of its own:
# a module's imports are host functions lib/wasm grants) and lib/wasm's glue.
# Configured by tools/wamr/build.sh.
cmake_minimum_required(VERSION 3.14)
project(rae_wamr C ASM)

set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_FAST_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_LIB_PTHREAD 0)
set(WAMR_BUILD_THREAD_MGR 1)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_SHARED_MEMORY 0)
set(WAMR_BUILD_BULK_MEMORY 1)
set(WAMR_BUILD_REF_TYPES 1)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_MINI_LOADER 0)
set(WAMR_BUILD_DUMP_CALL_STACK 0)

include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

add_library(rae_wamr STATIC ${WAMR_RUNTIME_LIB_SOURCE} ${CMAKE_CURRENT_LIST_DIR}/rae_wasm_glue.c)
target_include_directories(rae_wamr PRIVATE ${WAMR_ROOT_DIR}/core/iwasm/include ${CMAKE_CURRENT_LIST_DIR}/../../compiler/runtime)
