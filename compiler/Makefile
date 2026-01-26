# Rae Compiler Makefile

CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g
LDFLAGS = -lraylib -framework CoreVideo -framework IOKit -framework Cocoa -framework OpenGL -L/opt/homebrew/lib
CFLAGS += -I/opt/homebrew/include -I../third_party/tinyexpr

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
       $(SRC_DIR)/lexer.c \
       $(SRC_DIR)/ast.c \
       $(SRC_DIR)/parser.c \
       $(SRC_DIR)/pretty.c \
       $(SRC_DIR)/c_backend.c \
       $(SRC_DIR)/raepack.c \
       $(SRC_DIR)/vm_chunk.c \
       $(SRC_DIR)/vm_value.c \
       $(SRC_DIR)/vm.c \
       $(SRC_DIR)/vm_compiler.c \
       $(SRC_DIR)/vm_registry.c \
       $(SRC_DIR)/vm_raylib.c \
       $(SRC_DIR)/vm_tinyexpr.c

EXT_SRCS = ../third_party/tinyexpr/rae_tinyexpr.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
OBJS += $(BUILD_DIR)/rae_tinyexpr.o

TARGET = $(BIN_DIR)/rae
TEST_RUNNER = tools/run_tests.sh

.PHONY: all build test clean

all: build

build: $(TARGET)

$(BIN_DIR)/rae: $(BUILD_DIR)/main.o $(BUILD_DIR)/arena.o $(BUILD_DIR)/str.o $(BUILD_DIR)/diag.o $(BUILD_DIR)/lexer.o $(BUILD_DIR)/ast.o $(BUILD_DIR)/parser.o $(BUILD_DIR)/pretty.o $(BUILD_DIR)/c_backend.o $(BUILD_DIR)/raepack.o $(BUILD_DIR)/vm_chunk.o $(BUILD_DIR)/vm_value.o $(BUILD_DIR)/vm.o $(BUILD_DIR)/vm_compiler.o $(BUILD_DIR)/vm_registry.o $(BUILD_DIR)/vm_raylib.o $(BUILD_DIR)/vm_tinyexpr.o $(BUILD_DIR)/rae_tinyexpr.o $(BUILD_DIR)/sys_thread.o $(BUILD_DIR)/vm_patch.o $(BUILD_DIR)/rae_runtime.o
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

test: build
	@chmod +x $(TEST_RUNNER)
	@$(TEST_RUNNER) $(TEST)

clean:

	rm -rf $(BUILD_DIR) $(BIN_DIR) ../build

	@echo "Cleaned build artifacts"
