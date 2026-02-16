#!/bin/bash
# DEV_FAST: Rapid iteration script for Rae compiler

set -e

# Change to the compiler directory if not already there
cd "$(dirname "$0")/.."

echo "Building in DEV_FAST mode..."
make MODE=DEV_FAST build

echo "Running smoke tests..."
make smoke

echo "Success! Ready for next iteration."
