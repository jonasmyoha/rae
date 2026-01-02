#!/bin/bash
# Setup script for Rae programming language repository (monorepo structure)

set -e

echo "Setting up Rae programming language repository..."

# Create directory structure
mkdir -p compiler/src compiler/tests/cases compiler/tools spec examples docs

# Create main README
cat > README.md << 'EOF'
# Rae Programming Language

A programming language focused on explicit ownership semantics and natural syntax.

## Project Structure

- `compiler/` - The Rae compiler (C11 implementation)
- `spec/` - Language specification
- `examples/` - Example Rae programs
- `docs/` - Documentation

## Quick Start

```bash
cd compiler
make build
make test
```

## Status

**Phase 1 (MVP):** Lexer, parser, AST, pretty-printer ✅ In Progress

See `spec/rae.md` for language details.

## License

MIT
EOF

# Create spec
cat > spec/rae.md << 'EOF'
# Rae Language Specification

**Version:** 0.1 (WIP)
**Scope:** Lexer, parser, AST, pretty-printer (no codegen, no typechecking)

---

## 1. Lexical Structure

### 1.1 Comments

* **Line comment:** `# comment until newline`
* **Multiline comment:** `#[ comment across multiple lines ]#`

### 1.2 Identifiers

* Pattern: `[a-zA-Z_][a-zA-Z0-9_]*`
* Case-sensitive

### 1.3 Keywords

```
type func def ret spawn
own view mod opt
if else match case
true false none
and or not is
pub priv
```

### 1.4 Literals

* **Integer:** `0 | [1-9][0-9]*`
* **String:** `"text"` (supports `\n \t \\ \"`)
* **Boolean:** `true false`
* **None literal:** `none`

### 1.5 Operators and Punctuation

```
=  =>  +  -  *  /  %
<  >  <=  >=
( )  { }  [ ]
,  :  .
```

---

## 2. Core Concepts

### 2.1 Value vs Ownership

* **Plain type (`T`)** → value, copied by `=`
* **`own T`** → owning reference
* **`view T`** → read-only borrowed view
* **`mod T`** → mutable borrowed view
* **`opt T`** → optional wrapper for any type

### 2.2 Assignment and Binding

* **`=`** Always performs a **value copy**, never aliases
* **`=>`** Binds an owning reference (used only with `own`)

---

## 3. Types

### 3.1 Type Definition

```rae
type Point: pub {
  x: Int
  y: Int
}
```

Rules:
* Fields use `name: Type`
* `def` is **not allowed** in type fields

### 3.2 Optional Types

```rae
opt Int
opt own Texture
```

---

## 4. Functions

### 4.1 Function Definition

```rae
func add(a: view Point, b: view Point): pub ret Point {
  ret (x: a.x + b.x, y: a.y + b.y)
}
```

Rules:
* Parameters use `name: Type`
* `def` is **not allowed** in parameters
* Function properties (pub, spawn) come **after the parameter list**, space-separated
* Return declaration (`ret`) comes last

### 4.2 Multiple Return Values

```rae
func divide(a: Int, b: Int): pub ret result: Int, error: opt Error {
  if b is 0 {
    ret error: Error(message: "divide by zero")
  }
  ret result: a / b
}
```

### 4.3 Implicit `this`

* In any function, **`this` refers to the first parameter**

---

## 5. Concurrency

### 5.1 Spawn Functions

```rae
func heavyWork(id: Int): spawn {
  log("working")
}
```

* `spawn` is a **function property**

### 5.2 Spawn Calls

```rae
spawn heavyWork(id: 1)
```

---

## 6. Local Bindings

```rae
def x: Int = 10
def p: own Point => (x: 1, y: 2)
```

Rules:
* `def` is used **only** for locals
* Type annotation is required

---

## 7. Control Flow

### 7.1 If / Else

```rae
if x > 0 {
  log("positive")
} else {
  log("negative")
}
```

### 7.2 Match

```rae
match value {
  case 0 {
    log("zero")
  }
  case _ {
    log("other")
  }
}
```

---

## 8. Expressions

### 8.1 Operators

* Arithmetic: `+ - * / %`
* Comparison: `< > <= >= is`
* Logical: `and or not`

### 8.2 Member Access

```rae
point.x
```

### 8.3 Function Call

```rae
add(a: p, b: q)
spawn heavyWork(id: 2)
```

---

## 9. Constraints

1. `def` is **only** allowed for local bindings
2. `=` always copies by value
3. `=>` binds owning references
4. No semicolons
5. No implicit aliasing

---

**End of Rae Specification**
EOF

# Create compiler infrastructure
cat > compiler/src/arena.h << 'EOF'
/* arena.h - Bump allocator for compiler memory management */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

typedef struct Arena Arena;

Arena* arena_create(size_t capacity);
void* arena_alloc(Arena* a, size_t size);
void arena_reset(Arena* a);
void arena_destroy(Arena* a);
size_t arena_used(Arena* a);
size_t arena_capacity(Arena* a);

#endif /* ARENA_H */
EOF

cat > compiler/src/arena.c << 'EOF'
/* arena.c - Bump allocator implementation */

#include "arena.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define ARENA_ALIGN 8

struct Arena {
  char* buffer;
  size_t capacity;
  size_t used;
};

static size_t align_up(size_t n, size_t align) {
  return (n + align - 1) & ~(align - 1);
}

Arena* arena_create(size_t capacity) {
  Arena* a = malloc(sizeof(Arena));
  if (!a) return NULL;
  
  a->buffer = malloc(capacity);
  if (!a->buffer) {
    free(a);
    return NULL;
  }
  
  a->capacity = capacity;
  a->used = 0;
  return a;
}

void* arena_alloc(Arena* a, size_t size) {
  assert(a != NULL);
  
  size_t aligned_size = align_up(size, ARENA_ALIGN);
  
  if (a->used + aligned_size > a->capacity) {
    return NULL;
  }
  
  void* ptr = a->buffer + a->used;
  a->used += aligned_size;
  
  memset(ptr, 0, size);
  return ptr;
}

void arena_reset(Arena* a) {
  assert(a != NULL);
  a->used = 0;
}

void arena_destroy(Arena* a) {
  if (!a) return;
  free(a->buffer);
  free(a);
}

size_t arena_used(Arena* a) {
  assert(a != NULL);
  return a->used;
}

size_t arena_capacity(Arena* a) {
  assert(a != NULL);
  return a->capacity;
}
EOF

cat > compiler/src/str.h << 'EOF'
/* str.h - String slice and utility functions */

#ifndef STR_H
#define STR_H

#include <stddef.h>
#include <stdbool.h>

typedef struct {
  const char* data;
  size_t len;
} Str;

Str str_from_cstr(const char* cstr);
Str str_from_buf(const char* data, size_t len);
bool str_eq(Str a, Str b);
bool str_eq_cstr(Str a, const char* cstr);
char* str_to_cstr(Str s);
bool str_is_empty(Str s);
char* read_file(const char* path, size_t* out_size);

#endif /* STR_H */
EOF

cat > compiler/src/str.c << 'EOF'
/* str.c - String slice implementation */

#include "str.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

Str str_from_cstr(const char* cstr) {
  return (Str){.data = cstr, .len = strlen(cstr)};
}

Str str_from_buf(const char* data, size_t len) {
  return (Str){.data = data, .len = len};
}

bool str_eq(Str a, Str b) {
  if (a.len != b.len) return false;
  return memcmp(a.data, b.data, a.len) == 0;
}

bool str_eq_cstr(Str a, const char* cstr) {
  return str_eq(a, str_from_cstr(cstr));
}

char* str_to_cstr(Str s) {
  char* result = malloc(s.len + 1);
  if (!result) return NULL;
  
  memcpy(result, s.data, s.len);
  result[s.len] = '\0';
  return result;
}

bool str_is_empty(Str s) {
  return s.len == 0;
}

char* read_file(const char* path, size_t* out_size) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  fseek(f, 0, SEEK_SET);
  
  char* buffer = malloc(size + 1);
  if (!buffer) {
    fclose(f);
    return NULL;
  }
  
  size_t read = fread(buffer, 1, size, f);
  fclose(f);
  
  if (read != (size_t)size) {
    free(buffer);
    return NULL;
  }
  
  buffer[size] = '\0';
  if (out_size) *out_size = size;
  return buffer;
}
EOF

cat > compiler/src/diag.h << 'EOF'
/* diag.h - Diagnostic and error reporting */

#ifndef DIAG_H
#define DIAG_H

void diag_error(const char* file, int line, int col, const char* message);
void diag_fatal(const char* message);

#endif /* DIAG_H */
EOF

cat > compiler/src/diag.c << 'EOF'
/* diag.c - Diagnostic implementation */

#include "diag.h"
#include <stdio.h>
#include <stdlib.h>

void diag_error(const char* file, int line, int col, const char* message) {
  fprintf(stderr, "%s:%d:%d: %s\n", file, line, col, message);
  exit(1);
}

void diag_fatal(const char* message) {
  fprintf(stderr, "error: %s\n", message);
  exit(1);
}
EOF

cat > compiler/src/main.c << 'EOF'
/* main.c - Rae compiler entry point */

#include <stdio.h>
#include <string.h>
#include "arena.h"
#include "str.h"
#include "diag.h"

static void print_usage(const char* prog) {
  fprintf(stderr, "Usage: %s parse <file>\n", prog);
  fprintf(stderr, "\nCommands:\n");
  fprintf(stderr, "  parse <file>    Parse and pretty-print Rae source file\n");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }
  
  const char* cmd = argv[1];
  
  if (strcmp(cmd, "parse") == 0) {
    if (argc < 3) {
      fprintf(stderr, "error: parse command requires a file argument\n");
      print_usage(argv[0]);
      return 1;
    }
    
    const char* file_path = argv[2];
    
    size_t file_size;
    char* source = read_file(file_path, &file_size);
    if (!source) {
      fprintf(stderr, "error: could not read file '%s'\n", file_path);
      return 1;
    }
    
    Arena* arena = arena_create(1024 * 1024);
    if (!arena) {
      free(source);
      diag_fatal("could not allocate arena");
    }
    
    printf("Parse command received for file: %s\n", file_path);
    printf("File size: %zu bytes\n", file_size);
    printf("Arena capacity: %zu bytes\n", arena_capacity(arena));
    printf("\n[Parser not yet implemented]\n");
    
    arena_destroy(arena);
    free(source);
    return 0;
    
  } else {
    fprintf(stderr, "error: unknown command '%s'\n", cmd);
    print_usage(argv[0]);
    return 1;
  }
}
EOF

cat > compiler/Makefile << 'EOF'
# Rae Compiler Makefile

CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS =

SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin
TEST_DIR = tests

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/arena.c \
       $(SRC_DIR)/str.c \
       $(SRC_DIR)/diag.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
TARGET = $(BIN_DIR)/rae
TEST_RUNNER = tools/run_tests.sh

.PHONY: all build test clean

all: build

build: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "Built: $(TARGET)"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

test: build
	@chmod +x $(TEST_RUNNER)
	@$(TEST_RUNNER)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Cleaned build artifacts"
EOF

cat > compiler/tools/run_tests.sh << 'EOF'
#!/bin/bash
# Rae test runner

set -e

BIN="bin/rae"
TEST_DIR="tests/cases"
PASSED=0
FAILED=0

echo "Running Rae tests..."
echo

if [ ! -f "$BIN" ]; then
  echo "Error: $BIN not found. Run 'make build' first."
  exit 1
fi

if [ ! -d "$TEST_DIR" ]; then
  echo "Warning: $TEST_DIR not found. No tests to run."
  exit 0
fi

TEST_FILES=$(find "$TEST_DIR" -name "*.rae" | sort)

if [ -z "$TEST_FILES" ]; then
  echo "No test files found in $TEST_DIR"
  exit 0
fi

for TEST_FILE in $TEST_FILES; do
  TEST_NAME=$(basename "$TEST_FILE" .rae)
  EXPECT_FILE="${TEST_FILE%.rae}.expect"
  
  if [ ! -f "$EXPECT_FILE" ]; then
    echo "SKIP: $TEST_NAME (no .expect file)"
    continue
  fi
  
  ACTUAL_OUTPUT=$("$BIN" parse "$TEST_FILE" 2>&1 || true)
  EXPECTED_OUTPUT=$(cat "$EXPECT_FILE")
  
  if [ "$ACTUAL_OUTPUT" = "$EXPECTED_OUTPUT" ]; then
    echo "PASS: $TEST_NAME"
    ((PASSED++))
  else
    echo "FAIL: $TEST_NAME"
    echo "  Expected:"
    echo "$EXPECTED_OUTPUT" | sed 's/^/    /'
    echo "  Actual:"
    echo "$ACTUAL_OUTPUT" | sed 's/^/    /'
    echo
    ((FAILED++))
  fi
done

echo
echo "=========================================="
echo "Results: $PASSED passed, $FAILED failed"
echo "=========================================="

if [ $FAILED -gt 0 ]; then
  exit 1
fi
EOF

chmod +x compiler/tools/run_tests.sh

cat > compiler/tests/cases/000_smoke.rae << 'EOF'
# Smoke test - empty file
EOF

cat > compiler/tests/cases/000_smoke.expect << 'EOF'
Parse command received for file: tests/cases/000_smoke.rae
File size: 25 bytes
Arena capacity: 1048576 bytes

[Parser not yet implemented]
EOF

cat > compiler/.gitignore << 'EOF'
build/
bin/
*.o
*.swp
EOF

cat > compiler/README.md << 'EOF'
# Rae Compiler

C11 implementation of the Rae compiler (Phase 1: parse and pretty-print).

## Building

```bash
make build
```

## Testing

```bash
make test
```

## Usage

```bash
bin/rae parse <file>
```

See `../spec/rae.md` for language specification.
EOF

cat > examples/hello.rae << 'EOF'
# Hello world example

func main(): pub {
  log("Hello, Rae!")
}
EOF

cat > docs/design.md << 'EOF'
# Rae Design Philosophy

## Core Principles

1. **Explicit ownership** - No hidden aliasing or copying
2. **Natural syntax** - Reads like structured English
3. **AI-assisted** - Designed for clarity in AI collaboration

## Ownership System

- `own T` - Exclusive ownership
- `view T` - Read-only borrow
- `mod T` - Mutable borrow
- `opt T` - Optional wrapper

## Assignment Semantics

- `=` always copies values
- `=>` transfers ownership
- No implicit conversions
EOF

echo ""
echo "✓ Rae repository structure created!"
echo ""
echo "Structure:"
echo "  rae/"
echo "  ├── compiler/      # C compiler implementation"
echo "  ├── spec/          # Language specification"
echo "  ├── examples/      # Example programs"
echo "  └── docs/          # Documentation"
echo ""
echo "Next steps:"
echo "  cd compiler"
echo "  make build"
echo "  make test"
echo ""
