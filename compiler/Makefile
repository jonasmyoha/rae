# Rae Compiler Makefile

# Build modes: DEV_FAST (default) or CI_STRICT
MODE ?= DEV_FAST

CC = gcc
COMMON_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O0 -g
LDFLAGS = -framework Foundation -framework ImageIO -framework CoreGraphics -L/opt/homebrew/lib
COMMON_CFLAGS += -I/opt/homebrew/include -I../third_party/tinyexpr

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
       $(SRC_DIR)/arena.c \
       $(SRC_DIR)/str.c \
       $(SRC_DIR)/diag.c \
       $(SRC_DIR)/type.c \
       $(SRC_DIR)/sema.c \
       $(SRC_DIR)/array_methods.c \
       $(SRC_DIR)/lexer.c \
       $(SRC_DIR)/ast.c \
       $(SRC_DIR)/parser.c \
       $(SRC_DIR)/pretty.c \
       $(SRC_DIR)/pretty_writer.c \
       $(SRC_DIR)/pretty_expr.c \
       $(SRC_DIR)/pretty_decl.c \
       $(SRC_DIR)/rae_format.c \
       $(SRC_DIR)/rae_version.c \
       $(SRC_DIR)/rae_deps.c \
       $(SRC_DIR)/bindgen.c \
       $(SRC_DIR)/c_backend.c \
       $(SRC_DIR)/c_call.c \
       $(SRC_DIR)/c_discovery.c \
       $(SRC_DIR)/c_expr.c \
       $(SRC_DIR)/c_stmt.c \
       $(SRC_DIR)/ownership.c \
       $(SRC_DIR)/raepack.c \
       $(SRC_DIR)/mangler.c \
       $(SRC_DIR)/progress.c

EXT_SRCS = ../third_party/tinyexpr/rae_tinyexpr.c ../third_party/monocypher/monocypher.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
OBJS += $(BUILD_DIR)/rae_tinyexpr.o $(BUILD_DIR)/monocypher.o

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
TEST_RUNNER = tools/run_tests.sh
else
TEST_RUNNER = tools/run_tests_parallel.sh
endif

.PHONY: all build test test-examples stress clean smoke install uninstall c-surface-gate FORCE

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

$(BUILD_DIR)/monocypher.o: ../third_party/monocypher/monocypher.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-error -Wno-pedantic -c -o $@ $<

test: build
	@chmod +x $(TEST_RUNNER)
	@if [ -z "$(TEST)" ] || [ "$(TEST)" = "webgpuReadback" ]; then bash tools/test-webgpu-readback.sh; fi
	@if [ "$(TEST)" != "webgpuReadback" ]; then $(TEST_RUNNER) $(TEST); fi

# The VISUAL example gates (real SDL/GPU windows, screenshots, minutes of wall
# time). Deliberately NOT part of `make test`: run them on purpose, by hand.
test-examples: build
	@chmod +x tools/run_examples.sh
	@RAE_RUN_EXAMPLES=1 ./tools/run_examples.sh

# The stress cases (docs/stress-tests.md) on their own; `make test` runs them
# as one pre-suite case too.
stress: build
	@bash ../stress/run.sh

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
