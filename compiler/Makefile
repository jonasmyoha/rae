# Rae Compiler Makefile

# Build modes: DEV_FAST (default) or CI_STRICT
MODE ?= DEV_FAST

CC = gcc
# -O2 -flto: the compiler is optimized (O3 measured no faster than O2; LTO
# adds ~5-10% by inlining small cross-file helpers like str_eq). -g keeps
# symbols for profiling and debuggers; `make clean build COMPILER_OPT=-O0` gives an
# unoptimized build for stepping through code (objects do not track the
# flags, hence the clean).
# On Linux without LTO: gcc links it in one job, and gcc 13 aborted there under
# a parallel make in an emulated x86-64 (jobserver_info::return_token).
ifeq ($(shell uname -s),Darwin)
COMPILER_OPT ?= -O2 -flto
else
COMPILER_OPT ?= -O2
endif
COMMON_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic $(COMPILER_OPT) -g -ffp-contract=off
COMMON_CFLAGS += -I../third_party/tinyexpr
# The libraries of the machine the compiler is built on. Linux (glibc) hides
# POSIX and BSD names (PATH_MAX, realpath, kill) under -std=c11 without
# _GNU_SOURCE, and the last libraries on the line are the last searched.
ifeq ($(shell uname -s),Darwin)
COMMON_CFLAGS += -I/opt/homebrew/include
LDFLAGS = $(filter -flto,$(COMPILER_OPT)) -framework Foundation -framework ImageIO -framework CoreGraphics -L/opt/homebrew/lib
else
COMMON_CFLAGS += -D_GNU_SOURCE
LDFLAGS = $(filter -flto,$(COMPILER_OPT)) -lm -lpthread -ldl
endif

ifeq ($(MODE), CI_STRICT)
    CFLAGS = $(COMMON_CFLAGS) -Werror
else
    # Rapid iteration mode: keep critical errors, but don't stall on unused code
    CFLAGS = $(COMMON_CFLAGS) \
             -Wno-error=unused-variable \
             -Wno-error=unused-parameter \
             -Wno-error=unused-function \
             -Wno-error=unused-but-set-variable \
             -Wno-error=missing-field-initializers \
             -Wno-error=sign-compare
endif

SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin
TEST_DIR = tests
RUNTIME_DIR = $(abspath runtime)

CFLAGS += -DRAE_RUNTIME_SOURCE_DIR=\"$(RUNTIME_DIR)\"

# Header dependency tracking. Without this, the pattern rule below lists only
# the .c file, so editing a header does NOT rebuild the objects that include
# it. That is not a slow-build annoyance — a struct field added to ast.h left
# stale objects compiled against the OLD layout, and the mismatched reads
# produced plausible-looking garbage (a count that was really the ASCII bytes
# of a nearby string) rather than a crash. -MMD writes a .d file of real
# prerequisites per object; -MP adds phony targets for each header so a
# deleted/renamed header does not break the build with "No rule to make
# target".
CFLAGS += -MMD -MP

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/import_case.c \
       $(SRC_DIR)/module_folder.c \
       $(SRC_DIR)/arena.c \
       $(SRC_DIR)/str.c \
       $(SRC_DIR)/diag.c \
       $(SRC_DIR)/type.c \
       $(SRC_DIR)/sema.c \
       $(SRC_DIR)/sema_main_loop.c \
       $(SRC_DIR)/array_methods.c \
       $(SRC_DIR)/shader_compose.c \
       $(SRC_DIR)/lexer.c \
       $(SRC_DIR)/ast.c \
       $(SRC_DIR)/parser.c \
       $(SRC_DIR)/pretty.c \
       $(SRC_DIR)/pretty_writer.c \
       $(SRC_DIR)/pretty_expr.c \
       $(SRC_DIR)/pretty_decl.c \
       $(SRC_DIR)/rae_format.c \
       $(SRC_DIR)/rae_version.c \
       $(SRC_DIR)/runtime_cache.c \
       $(SRC_DIR)/type_names.c \
       $(SRC_DIR)/may_wait.c \
       $(SRC_DIR)/may_wait_walk.c \
       $(SRC_DIR)/may_wait_block.c \
       $(SRC_DIR)/may_wait_report.c \
       $(SRC_DIR)/realtime_check.c \
       $(SRC_DIR)/when_resolve.c \
       $(SRC_DIR)/rae_deps.c \
       $(SRC_DIR)/rae_pack_edit.c \
       $(SRC_DIR)/rae_toolchain.c \
       $(SRC_DIR)/rae_package_native.c \
       $(SRC_DIR)/bindgen.c \
       $(SRC_DIR)/bindgen_preprocess.c \
       $(SRC_DIR)/c_backend.c \
       $(SRC_DIR)/c_call.c \
       $(SRC_DIR)/c_discovery.c \
       $(SRC_DIR)/c_expr.c \
       $(SRC_DIR)/c_loop_versioning.c \
       $(SRC_DIR)/c_stmt.c \
       $(SRC_DIR)/c_twin.c \
       $(SRC_DIR)/c_module_exports.c \
       $(SRC_DIR)/c_binary.c \
       $(SRC_DIR)/c_lend.c \
       $(SRC_DIR)/c_blocking.c \
       $(SRC_DIR)/c_names.c \
       $(SRC_DIR)/c_decl_index.c \
       $(SRC_DIR)/c_struct_shapes.c \
       $(SRC_DIR)/c_map_helpers.c \
       $(SRC_DIR)/c_task_result.c \
       $(SRC_DIR)/c_discard.c \
       $(SRC_DIR)/c_array_helpers.c \
       $(SRC_DIR)/ownership.c \
       $(SRC_DIR)/raepack.c \
       $(SRC_DIR)/mangler.c \
       $(SRC_DIR)/progress.c

EXT_SRCS = ../third_party/tinyexpr/rae_tinyexpr.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
OBJS += $(BUILD_DIR)/rae_tinyexpr.o

DEPS = $(OBJS:.o=.d)
# rae_runtime.o and sys_thread.o are NOT in $(OBJS) (they hang off the bin/rae
# rule directly), so their generated .d files must be listed here explicitly or
# they are never -include'd. Without this, rae_runtime.c's ~30 `#include`d
# runtime_*.c files are invisible to make: editing one and running `make` did
# nothing, and only `make clean && make` picked the edit up (#989).
DEPS += $(BUILD_DIR)/rae_runtime.d $(BUILD_DIR)/sys_thread.d

TARGET = $(BIN_DIR)/rae
# Test runner selection. The PARALLEL per-case runner (#824) is the DEFAULT for
# `make test` (and so tools/watch-tests.sh + the devtools Test Runner tab): it
# gives byte-identical verdicts to the serial runner at ~2.7x on 10 cores.
# The sequential runner is a debugging fallback only (e.g. isolating a runner
# bug or a machine where parallelism confuses things) — force it with
# RAE_TEST_SEQUENTIAL=1. RAE_TEST_PARALLEL=1 is still accepted (now a no-op,
# parallel being the default) so older invocations keep working.
ifeq ($(RAE_TEST_SEQUENTIAL),1)
TEST_RUNNER = $(CURDIR)/tools/run_tests.sh
else
TEST_RUNNER = $(CURDIR)/tools/run_tests_parallel.sh
endif

.PHONY: ui-model-gate world-input-gate wasm-stack-check module-folder-tools realtime-check wasm-audio-check toolchain-store-test c-warnings-gate wasm-module-check all build test test-examples checks stress clean smoke install uninstall c-surface-gate FORCE

all: build

build: $(TARGET)

# Version info (#930, docs/versioning-and-toolchain.md §1). Regenerated on
# every build from VERSION + git state; rewritten only when its content
# actually changes, so touching it does not force an unrelated rebuild of
# every object that happens to include it (just main.o, which reads it).
VERSION_HEADER = $(BUILD_DIR)/version_gen.h

$(VERSION_HEADER): FORCE
	@mkdir -p $(BUILD_DIR)
	@bash tools/gen-version-header.sh > $(VERSION_HEADER).tmp
	@cmp -s $(VERSION_HEADER).tmp $(VERSION_HEADER) 2>/dev/null || mv $(VERSION_HEADER).tmp $(VERSION_HEADER)
	@rm -f $(VERSION_HEADER).tmp

# `make install` — symlink the built rae binary into a PATH directory
# so plain `rae init` / `rae watch` / etc. work from any shell. Defaults
# to ~/.local/bin (per the XDG-ish convention shipped on modern macOS
# / Linux dotfiles); override with `make install INSTALL_DIR=/usr/local/bin`.
# Idempotent: re-running rebuilds the symlink to point at the current
# bin/rae, picking up rebuilds automatically. The target itself depends
# on `build` so a fresh `make install` builds first.
INSTALL_DIR ?= $(HOME)/.local/bin
INSTALL_LINK := $(INSTALL_DIR)/rae

install: build
	@mkdir -p $(INSTALL_DIR)
	@ln -sfn $(abspath $(TARGET)) $(INSTALL_LINK)
	@echo "linked $(INSTALL_LINK) -> $(abspath $(TARGET))"
	@case ":$$PATH:" in \
	  *":$(INSTALL_DIR):"*) echo "$(INSTALL_DIR) is on PATH; \`rae init\` should work now.";; \
	  *) echo "WARNING: $(INSTALL_DIR) is NOT on your PATH. Add this to ~/.zshrc:"; \
	     echo "  export PATH=\"$(INSTALL_DIR):\$$PATH\"";; \
	esac

uninstall:
	@rm -f $(INSTALL_LINK)
	@echo "removed $(INSTALL_LINK)"

$(BIN_DIR)/rae: $(OBJS) $(BUILD_DIR)/sys_thread.o $(BUILD_DIR)/rae_runtime.o
	@mkdir -p $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/rae_runtime.o: runtime/rae_runtime.c runtime/rae_runtime.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/sys_thread.o: src/sys_thread.c src/sys_thread.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/main.o: src/main.c $(VERSION_HEADER)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/rae_tinyexpr.o: ../third_party/tinyexpr/rae_tinyexpr.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-error -Wno-pedantic -Wno-gnu-zero-variadic-macro-arguments -c -o $@ $<

test: build
	@chmod +x $(TEST_RUNNER)
	@if [ -z "$(TEST)" ] || [ "$(TEST)" = "webgpuReadback" ]; then bash tools/test-webgpu-readback.sh; fi
	@if [ "$(TEST)" != "webgpuReadback" ]; then $(TEST_RUNNER) $(TEST); fi

# The VISUAL example gates (real SDL/GPU windows, screenshots, minutes of wall
# time). Deliberately NOT part of `make test`: run them on purpose, by hand.
test-examples: build
	@chmod +x tools/run_examples.sh
	@RAE_RUN_EXAMPLES=1 ./tools/run_examples.sh

# The hardware checks (checks/README.md): programs that test the GPU layer on
# a real GPU and exit 0 when they pass. `make test-examples` runs them too.
checks: build
	@bash ../checks/run.sh

# The stress cases (docs/stress-tests.md) on their own; `make test` runs them
# as one pre-suite case too.
stress: build
	@bash ../stress/run.sh

# The threaded fixtures (and 106's workers) under ThreadSanitizer
# (tools/tsan-check.sh); `make test` runs it as one pre-suite case too.
tsan: build
	@bash tools/tsan-check.sh

# The C side of realtime / noAlloc (docs/realtime-language-design.md §5):
# -Wfunction-effects on the real-time fixtures' C, and RealtimeSanitizer runs
# (LLVM's clang; SKIP without it). `make test` runs it as a pre-suite case too.
realtime-check: build
	@bash tools/realtime-sanitize-check.sh

# The audio package in the browser (docs/audio-design.md §11 step 3): its checks in headless
# Chrome, muted. About a minute; not part of the full run.
wasm-audio-check: build
	@bash tools/wasm_audio_check.sh

# The toolchain store and ./rae, from a fresh clone with an empty RAE_HOME. About two minutes,
# which the full run cannot spare, so it is not part of it: run it when they change.
toolchain-store-test:
	@bash tools/test-toolchain-store.sh

# The memory-boundary fixtures under AddressSanitizer + UBSan
# (tools/asan-check.sh); `make test` runs it as one pre-suite case too.
asan: build
	@bash tools/asan-check.sh

# Every benchmark program still compiles (tools/benchmarks-compile-check.sh);
# `make test` runs it as one pre-suite case too.
benchmarks-check: build
	@bash tools/benchmarks-compile-check.sh

# lib/Float8 built for x86-64 (with and without AVX2) and for wasm
# (tools/float8-lowerings-check.sh); `make test` runs it as one case too.
float8-lowerings: build
	@bash tools/float8-lowerings-check.sh

# The generated C of the largest and the browser-smoke apps compiles with
# -Wall -Wextra -Werror (tools/c-warnings-gate.sh); `make test` runs it as one
# pre-suite case too.
# The tools that work per file also work per module folder (docs/module-model-options.md)
module-folder-tools: build
	bash tools/module-folder-tools-check.sh

ui-model-gate:
	bash tools/ui-model-gate.sh

world-input-gate:
	bash tools/world-input-gate.sh

wasm-stack-check:
	bash tools/wasm-stack-check.sh

c-warnings-gate: build
	@bash tools/c-warnings-gate.sh

# A WASM module (docs/wasm-modules.md) built and called from Node
# (tools/wasm-module-check.sh); `make test` runs it as one pre-suite case.
wasm-module-check: build
	@bash tools/wasm-module-check.sh

smoke: build
	@chmod +x tools/smoke.sh
	@./tools/smoke.sh

# WebGPU C-surface gate (#505): reject new renderer-specific C helpers.
# See docs/webgpu-c-surface-audit.md.
c-surface-gate:
	@sh ../tools/webgpu-c-surface-gate.sh

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) ../build
	@echo "Cleaned build artifacts"

# Pull in the generated per-object header dependencies. Placed last, and with
# a leading `-` so the very first build (when no .d files exist yet) is not an
# error.
-include $(DEPS)
