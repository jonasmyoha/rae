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
  case "$TEST_FILE" in
    */helpers/*)
      continue
      ;;
  esac
  TEST_NAME=$(basename "$TEST_FILE" .rae)
  EXPECT_FILE="${TEST_FILE%.rae}.expect"
  
  if [ ! -f "$EXPECT_FILE" ]; then
    echo "SKIP: $TEST_NAME (no .expect file)"
    continue
  fi
  
  CMD_FILE="${TEST_FILE%.rae}.cmd"
  CMD_ARGS=("parse")
  CMD_LINE=""
  if [ -f "$CMD_FILE" ]; then
    CMD_LINE=$(head -n 1 "$CMD_FILE" | tr -d '\r')
    CMD_LINE=$(echo "$CMD_LINE" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    if [ -n "$CMD_LINE" ]; then
      read -r -a CMD_ARGS <<< "$CMD_LINE"
    fi
  fi
  if [ ${#CMD_ARGS[@]} -eq 0 ]; then
    CMD_ARGS=("parse")
  fi

  TMP_OUTPUT_FILE=""
  TMP_INPUT_FILE=""
  APPEND_TEST_FILE=1
  for idx in "${!CMD_ARGS[@]}"; do
    if [ "${CMD_ARGS[$idx]}" = "{{TMP_OUTPUT}}" ]; then
      if [ -z "$TMP_OUTPUT_FILE" ]; then
        TMP_OUTPUT_FILE=$(mktemp)
      fi
      CMD_ARGS[$idx]="$TMP_OUTPUT_FILE"
    elif [ "${CMD_ARGS[$idx]}" = "{{TMP_INPUT}}" ]; then
      if [ -z "$TMP_INPUT_FILE" ]; then
        TMP_INPUT_FILE=$(mktemp)
        cp "$TEST_FILE" "$TMP_INPUT_FILE"
      fi
      CMD_ARGS[$idx]="$TMP_INPUT_FILE"
      APPEND_TEST_FILE=0
    fi
  done

  CMD_RUN_ARGS=("${CMD_ARGS[@]}")
  if [ $APPEND_TEST_FILE -eq 1 ]; then
    CMD_RUN_ARGS+=("$TEST_FILE")
  fi

  CMD_STDOUT=$("$BIN" "${CMD_RUN_ARGS[@]}" 2>&1 || true)
  ACTUAL_OUTPUT="$CMD_STDOUT"
  if [ -n "$TMP_OUTPUT_FILE" ]; then
    if [ ! -f "$TMP_OUTPUT_FILE" ]; then
      echo "FAIL: $TEST_NAME (expected formatter output file '$TMP_OUTPUT_FILE')"
      ((FAILED++))
      rm -f "$TMP_OUTPUT_FILE"
      if [ -n "$TMP_INPUT_FILE" ]; then
        rm -f "$TMP_INPUT_FILE"
      fi
      continue
    fi
    if [ -n "$CMD_STDOUT" ]; then
      echo "FAIL: $TEST_NAME (expected no stdout when capturing formatted output)"
      echo "  Stdout:"
      echo "$CMD_STDOUT" | sed 's/^/    /'
      ((FAILED++))
      rm -f "$TMP_OUTPUT_FILE"
      if [ -n "$TMP_INPUT_FILE" ]; then
        rm -f "$TMP_INPUT_FILE"
      fi
      continue
    fi
    ACTUAL_OUTPUT=$(cat "$TMP_OUTPUT_FILE")
    rm -f "$TMP_OUTPUT_FILE"
    if [ -n "$TMP_INPUT_FILE" ]; then
      rm -f "$TMP_INPUT_FILE"
    fi
  elif [ -n "$TMP_INPUT_FILE" ]; then
    if [ -n "$CMD_STDOUT" ]; then
      echo "FAIL: $TEST_NAME (expected no stdout when rewriting input copy)"
      echo "  Stdout:"
      echo "$CMD_STDOUT" | sed 's/^/    /'
      ((FAILED++))
      rm -f "$TMP_INPUT_FILE"
      continue
    fi
    ACTUAL_OUTPUT=$(cat "$TMP_INPUT_FILE")
    rm -f "$TMP_INPUT_FILE"
  fi

  EXPECTED_OUTPUT=$(cat "$EXPECT_FILE")
  CMD_NAME="${CMD_ARGS[0]}"
  SIMPLE_FORMAT=0
  if [ "$CMD_NAME" = "format" ] && [ ${#CMD_ARGS[@]} -eq 1 ]; then
    SIMPLE_FORMAT=1
  fi
  
  if [ "$ACTUAL_OUTPUT" = "$EXPECTED_OUTPUT" ]; then
    if [ $SIMPLE_FORMAT -eq 1 ]; then
      IDEMP_OUTPUT=$("$BIN" format "$EXPECT_FILE" 2>&1 || true)
      if [ "$IDEMP_OUTPUT" != "$EXPECTED_OUTPUT" ]; then
        echo "FAIL: $TEST_NAME (format not idempotent)"
        echo "  Expected canonical output to remain unchanged when formatting .expect file"
        ((FAILED++))
        continue
      fi
      AST_ORIG=$("$BIN" parse "$TEST_FILE" 2>&1 || true)
      AST_FORMATTED=$("$BIN" parse "$EXPECT_FILE" 2>&1 || true)
      if [ "$AST_ORIG" != "$AST_FORMATTED" ]; then
        echo "FAIL: $TEST_NAME (AST mismatch after formatting)"
        echo "  Original AST:"
        echo "$AST_ORIG" | sed 's/^/    /'
        echo "  Formatted AST:"
        echo "$AST_FORMATTED" | sed 's/^/    /'
        ((FAILED++))
        continue
      fi
    fi
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
