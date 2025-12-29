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
  
  CMD_FILE="${TEST_FILE%.rae}.cmd"
  CMD="parse"
  if [ -f "$CMD_FILE" ]; then
    CMD=$(head -n 1 "$CMD_FILE" | tr -d '\r')
    CMD=$(echo "$CMD" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    if [ -z "$CMD" ]; then
      CMD="parse"
    fi
  fi

  ACTUAL_OUTPUT=$("$BIN" "$CMD" "$TEST_FILE" 2>&1 || true)
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
