# Rae Compiler Makefile

# Build modes: DEV_FAST (default) or CI_STRICT
MODE ?= DEV_FAST

CC = gcc
COMMON_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O0 -g
# Link raylib statically so the GLFW symbols bundled inside libraylib.a
# (glfwWaitEventsTimeout, glfwPostEmptyEvent, ...) are resolvable.
# The shared library libraylib.dylib does not export them.
LDFLAGS = /opt/homebrew/lib/libraylib.a -framework CoreVideo -framework IOKit -framework Cocoa -framework OpenGL -framework ImageIO -framework CoreGraphics -L/opt/homebrew/lib
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

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/arena.c \
       $(SRC_DIR)/str.c \
       $(SRC_DIR)/diag.c \
       $(SRC_DIR)/type.c \
       $(SRC_DIR)/sema.c \
       $(SRC_DIR)/lexer.c \
       $(SRC_DIR)/ast.c \
       $(SRC_DIR)/parser.c \
       $(SRC_DIR)/pretty.c \
       $(SRC_DIR)/c_backend.c \
       $(SRC_DIR)/c_call.c \
       $(SRC_DIR)/c_discovery.c \
       $(SRC_DIR)/c_expr.c \
       $(SRC_DIR)/c_stmt.c \
       $(SRC_DIR)/ownership.c \
       $(SRC_DIR)/vm_drop.c \
       $(SRC_DIR)/raepack.c \
       $(SRC_DIR)/vm_chunk.c \
       $(SRC_DIR)/vm_value.c \
       $(SRC_DIR)/vm.c \
       $(SRC_DIR)/vm_compiler.c \
       $(SRC_DIR)/vm_emit_expr.c \
       $(SRC_DIR)/vm_emit_stmt.c \
       $(SRC_DIR)/vm_natives_core.c \
       $(SRC_DIR)/vm_registry.c \
       $(SRC_DIR)/vm_raylib.c \
       $(SRC_DIR)/vm_tinyexpr.c \
       $(SRC_DIR)/vm_monocypher.c \
       $(SRC_DIR)/mangler.c

EXT_SRCS = ../third_party/tinyexpr/rae_tinyexpr.c ../third_party/monocypher/monocypher.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
OBJS += $(BUILD_DIR)/rae_tinyexpr.o $(BUILD_DIR)/monocypher.o

TARGET = $(BIN_DIR)/rae
TEST_RUNNER = tools/run_tests.sh

.PHONY: all build test clean smoke install uninstall

all: build

build: $(TARGET)

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

$(BIN_DIR)/rae: $(BUILD_DIR)/main.o $(BUILD_DIR)/arena.o $(BUILD_DIR)/str.o $(BUILD_DIR)/diag.o $(BUILD_DIR)/type.o $(BUILD_DIR)/sema.o $(BUILD_DIR)/lexer.o $(BUILD_DIR)/ast.o $(BUILD_DIR)/parser.o $(BUILD_DIR)/pretty.o $(BUILD_DIR)/c_backend.o $(BUILD_DIR)/c_call.o $(BUILD_DIR)/c_discovery.o $(BUILD_DIR)/c_expr.o $(BUILD_DIR)/c_stmt.o $(BUILD_DIR)/ownership.o $(BUILD_DIR)/vm_drop.o $(BUILD_DIR)/raepack.o $(BUILD_DIR)/vm_chunk.o $(BUILD_DIR)/vm_value.o $(BUILD_DIR)/vm.o $(BUILD_DIR)/vm_compiler.o $(BUILD_DIR)/vm_emit_expr.o $(BUILD_DIR)/vm_emit_stmt.o $(BUILD_DIR)/vm_natives_core.o $(BUILD_DIR)/vm_registry.o $(BUILD_DIR)/vm_raylib.o $(BUILD_DIR)/vm_tinyexpr.o $(BUILD_DIR)/mangler.o $(BUILD_DIR)/vm_monocypher.o $(BUILD_DIR)/rae_tinyexpr.o $(BUILD_DIR)/monocypher.o $(BUILD_DIR)/sys_thread.o $(BUILD_DIR)/vm_patch.o $(BUILD_DIR)/rae_runtime.o
	@mkdir -p $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/rae_runtime.o: runtime/rae_runtime.c runtime/rae_runtime.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/vm_patch.o: src/vm_patch.c src/vm.h src/vm_chunk.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/sys_thread.o: src/sys_thread.c src/sys_thread.h
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
	@$(TEST_RUNNER) $(TEST)

smoke: build
	@chmod +x tools/smoke.sh
	@./tools/smoke.sh

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) ../build
	@echo "Cleaned build artifacts"
