#!/bin/bash
# Rae smoke test - fast sanity check (< 5s)

set -e

BIN="./bin/rae"
TEST_RUNNER="./tools/run_tests.sh"

echo "--- RAE SMOKE TEST ---"

# 1. Parse check
echo "Check: Parsing stdlib..."
$BIN parse ../lib/core.rae > /dev/null

# 2. VM Smoke (Hello World)
echo "Check: VM execution..."
./tools/run_tests.sh 300 > /dev/null

# 3. C Backend Smoke
echo "Check: C Backend emission & compile..."
TARGET=compiled ./tools/run_tests.sh 300 > /dev/null

# 4. Collection/Generics check
echo "Check: Generics & Collections (VM)..."
./tools/run_tests.sh 325 > /dev/null

echo "Check: Generics & Collections (C)..."
TARGET=compiled ./tools/run_tests.sh 325 > /dev/null

echo "--- SMOKE TEST PASSED ---"
