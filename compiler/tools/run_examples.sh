#!/bin/bash
# Rae example smoke tester - verifies all examples compile

set -e

BIN="bin/rae"
EXAMPLES_DIR="../examples"
PASSED=0
FAILED=0

echo "Running Rae example smoke tests..."
echo

if [ ! -f "$BIN" ]; then
  echo "Error: $BIN not found. Run 'make build' first."
  exit 1
fi

# Find all main.rae files in examples subdirectories
EXAMPLE_FILES=$(find "$EXAMPLES_DIR" -name "main.rae" | sort)

for EXAMPLE_FILE in $EXAMPLE_FILES; do
  EXAMPLE_NAME=$(basename "$(dirname "$EXAMPLE_FILE")")
  PROJECT_DIR=$(dirname "$EXAMPLE_FILE")

  # 1. VM Compile Smoke Test
  if "$BIN" build --target live --project "$PROJECT_DIR" "$EXAMPLE_FILE" > /dev/null 2>&1; then
    # 2. C Backend Smoke Test (generate + compile + link)
    TMP_OUT=$(mktemp -d)
    if "$BIN" build --target compiled --emit-c --project "$PROJECT_DIR" --out "$TMP_OUT/out.c" "$EXAMPLE_FILE" > "$TMP_OUT/emit.log" 2>&1; then
      # Attempt full compilation
      # Note: we include Raylib flags since many examples use it.
      if gcc -O2 -o "$TMP_OUT/app" "$TMP_OUT/out.c" "$TMP_OUT/rae_runtime.c" \
         $(ls "$PROJECT_DIR"/*.c 2>/dev/null | grep -v "rae_runtime.c" | grep -v "main_compiled.c" || true) \
         -I"$TMP_OUT" -I/opt/homebrew/include -L/opt/homebrew/lib -DRAE_HAS_RAYLIB \
         -lraylib -framework CoreVideo -framework IOKit -framework Cocoa -framework OpenGL > "$TMP_OUT/link.log" 2>&1; then
        echo "PASS: $EXAMPLE_NAME"
        ((PASSED++))
      else
        echo "FAIL: $EXAMPLE_NAME (C linking)"
        cat "$TMP_OUT/link.log" | sed 's/^/  /'
        ((FAILED++))
      fi
    else
      echo "FAIL: $EXAMPLE_NAME (C backend emit)"
      cat "$TMP_OUT/emit.log" | sed 's/^/  /'
      ((FAILED++))
    fi
    rm -rf "$TMP_OUT"
  else
    echo "FAIL: $EXAMPLE_NAME (VM compiler)"
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
