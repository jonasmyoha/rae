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

  # 1. VM Compile Smoke Test
  if "$BIN" build --target live "$EXAMPLE_FILE" > /dev/null 2>&1; then
    # 2. C Backend Smoke Test
    if "$BIN" build --target compiled --emit-c "$EXAMPLE_FILE" > /dev/null 2>&1; then
      echo "PASS: $EXAMPLE_NAME"
      ((PASSED++))
    else
      echo "FAIL: $EXAMPLE_NAME (C backend)"
      ((FAILED++))
    fi
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
