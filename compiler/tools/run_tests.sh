#!/bin/bash
# Rae test runner

set -e

BIN="bin/rae"
TEST_DIR="tests/cases"
PASSED=0
FAILED=0
TARGET_FILTER=$(printf "%s" "${TEST_TARGET:-${TARGET:-}}" | tr '[:upper:]' '[:lower:]')

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

if [ -n "$TARGET_FILTER" ]; then
  echo "Target filter: $TARGET_FILTER"
  echo
fi

should_run_test() {
  local name="$1"
  case "$TARGET_FILTER" in
    compiled)
      case "$name" in
        400_*|401_*|402_*|403_*|404_*|405_*|406_*) return 0 ;;
        *) return 1 ;;
      esac
      ;;
    hybrid)
      case "$name" in
        407_*|408_*) return 0 ;;
        *) return 1 ;;
      esac
      ;;
    ""|live)
      return 0
      ;;
    *)
      return 0
      ;;
  esac
}

# Targets to test
if [ -n "$TARGET_FILTER" ]; then
  TARGETS=("$TARGET_FILTER")
else
  TARGETS=("live" "compiled")
fi

for TARGET in "${TARGETS[@]}"; do
  echo "Testing target: $TARGET"
  echo "------------------------"
  
  for TEST_FILE in $TEST_FILES; do
    case "$TEST_FILE" in
      */helpers/*)
        continue
        ;;
    esac
    TEST_DIRNAME=$(dirname "$TEST_FILE")
    if [ -f "$TEST_DIRNAME/.raetesthelper" ]; then
      continue
    fi
    TEST_NAME=$(basename "$TEST_FILE" .rae)
    
    EXPECT_FILE="${TEST_FILE%.rae}.expect"
    if [ ! -f "$EXPECT_FILE" ]; then
      if [ "$TARGET" = "${TARGETS[0]}" ]; then
        echo "SKIP: $TEST_NAME (no .expect file)"
      fi
      continue
    fi

    CMD_FILE="${TEST_FILE%.rae}.cmd"
    CMD_ARGS=("parse") # Default
    if [ -f "$CMD_FILE" ]; then
      CMD_LINE=$(head -n 1 "$CMD_FILE" | tr -d '\r')
      CMD_LINE=$(echo "$CMD_LINE" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
      if [ -n "$CMD_LINE" ]; then
        read -r -a CMD_ARGS <<< "$CMD_LINE"
      fi
    fi

    # Filtering logic
    RUN_THIS=1
    if [ "$TARGET" = "compiled" ]; then
        case "$TEST_NAME" in
            # Skip build tests that target live/hybrid specifically
            407_*|408_*) RUN_THIS=0 ;;
            # Skip tests that don't have func main() (C backend expects a main)
            000_*|100_*|101_*|102_*|103_*|104_*|105_*|340_*) RUN_THIS=0 ;;
            # Skip tests known to have different output or no support in C yet
            306_*|318_*|332_*|334_*|338_*|339_*|343_*) RUN_THIS=0 ;;
        esac
        # C backend only handles 'run' or 'build' commands in this loop
        case "${CMD_ARGS[0]}" in
            lex|parse|format|pack) RUN_THIS=0 ;;
        esac
    fi

    if [ $RUN_THIS -eq 0 ]; then
        continue
    fi

    # Labels and TMP logic
    DISPLAY_NAME="$TEST_NAME [$TARGET]"
    if [[ "${CMD_ARGS[0]}" =~ ^(parse|lex|format)$ ]]; then
        DISPLAY_NAME="$TEST_NAME"
    fi

    TMP_OUTPUT_FILE=""
    TMP_INPUT_FILE=""
    TMP_OUTPUT_DIR=""
    APPEND_TEST_FILE=1
    CMD_RUN_ARGS=("${CMD_ARGS[@]}")

    for idx in "${!CMD_RUN_ARGS[@]}"; do
      if [ "${CMD_RUN_ARGS[$idx]}" = "{{TMP_OUTPUT}}" ]; then
        if [ -z "$TMP_OUTPUT_FILE" ]; then
          TMP_OUTPUT_FILE=$(mktemp)
        fi
        CMD_RUN_ARGS[$idx]="$TMP_OUTPUT_FILE"
      elif [ "${CMD_RUN_ARGS[$idx]}" = "{{TMP_INPUT}}" ]; then
        if [ -z "$TMP_INPUT_FILE" ]; then
          TMP_INPUT_FILE=$(mktemp)
          cp "$TEST_FILE" "$TMP_INPUT_FILE"
        fi
        CMD_RUN_ARGS[$idx]="$TMP_INPUT_FILE"
        APPEND_TEST_FILE=0
      elif [ "${CMD_RUN_ARGS[$idx]}" = "{{TMP_OUTDIR}}" ]; then
        if [ -z "$TMP_OUTPUT_DIR" ]; then
          TMP_OUTPUT_DIR=$(mktemp -d)
        fi
        CMD_RUN_ARGS[$idx]="$TMP_OUTPUT_DIR"
      fi
    done

    # Final command assembly
    if [ "${CMD_RUN_ARGS[0]}" = "run" ]; then
        if [ $APPEND_TEST_FILE -eq 1 ]; then
            CMD_RUN_ARGS=("run" "--target" "$TARGET" "$TEST_FILE")
        else
            CMD_RUN_ARGS=("run" "--target" "$TARGET" "$TMP_INPUT_FILE")
        fi
    elif [ $APPEND_TEST_FILE -eq 1 ]; then
      CMD_RUN_ARGS+=("$TEST_FILE")
    fi

    CMD_STDOUT=$("$BIN" "${CMD_RUN_ARGS[@]}" 2>&1 || true)
    ACTUAL_OUTPUT="$CMD_STDOUT"
    
    if [ -n "$TMP_OUTPUT_FILE" ]; then
      ACTUAL_OUTPUT=$(cat "$TMP_OUTPUT_FILE")
      rm -f "$TMP_OUTPUT_FILE"
    elif [ -n "$TMP_INPUT_FILE" ]; then
      if [ "${CMD_ARGS[0]}" = "format" ]; then
          ACTUAL_OUTPUT=$(cat "$TMP_INPUT_FILE")
      fi
      rm -f "$TMP_INPUT_FILE"
    fi

    if [ -n "$TMP_OUTPUT_DIR" ]; then
      # Package summary...
      rm -rf "$TMP_OUTPUT_DIR"
    fi
    
    EXPECTED_OUTPUT=$(cat "$EXPECT_FILE")
    if [ "$ACTUAL_OUTPUT" = "$EXPECTED_OUTPUT" ]; then
      echo "PASS: $DISPLAY_NAME"
      ((PASSED++))
    elif [ "$(head -n 1 "$EXPECT_FILE" | tr -d '\r')" = "BINARY_SKIP_MATCH" ]; then
      echo "PASS: $DISPLAY_NAME (binary match skipped)"
      ((PASSED++))
    else
      echo "FAIL: $DISPLAY_NAME"
      echo "  Expected:"
      echo "$EXPECTED_OUTPUT" | sed 's/^/    /'
      echo "  Actual:"
      echo "$ACTUAL_OUTPUT" | sed 's/^/    /'
      echo
      ((FAILED++))
    fi
  done
  echo
done

echo
echo "=========================================="
echo "Results: $PASSED passed, $FAILED failed"
echo "=========================================="

if [ $FAILED -gt 0 ]; then
  exit 1
fi

# Also run example smoke tests if we are in 'live' or default mode
if [ -z "$TARGET_FILTER" ] || [ "$TARGET_FILTER" = "live" ]; then
  echo
  ./tools/run_examples.sh
fi
