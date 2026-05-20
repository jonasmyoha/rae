#!/bin/bash
# Hot reload demo script for Rae

EXAMPLE_DIR=$(cd "$(dirname "$0")" && pwd)
MAIN_RAE="$EXAMPLE_DIR/main.rae"
# Try to find the compiler
if [ -f "./compiler/bin/rae" ]; then
  COMPILER="./compiler/bin/rae"
elif [ -f "./rae/compiler/bin/rae" ]; then
  COMPILER="./rae/compiler/bin/rae"
elif [ -f "../../compiler/bin/rae" ]; then
  COMPILER="../../compiler/bin/rae"
else
  echo "❌ Error: Could not find rae compiler binary."
  exit 1
fi

echo "🚀 Starting Hot Reload Demo..."
echo "1. Launching watcher in background..."
$COMPILER run --watch "$MAIN_RAE" &
WATCHER_PID=$!

# Ensure we kill the watcher on exit
trap "kill $WATCHER_PID 2>/dev/null" EXIT

sleep 4
echo ""
echo "2. Modifying file: changing '2.0' to 'HOT RELOADED!'"
# Portable-ish sed
if [[ "$OSTYPE" == "darwin"* ]]; then
  sed -i '' 's/RAE 2.0/HOT RELOADED!/' "$MAIN_RAE"
else
  sed -i 's/RAE 2.0/HOT RELOADED!/' "$MAIN_RAE"
fi

echo "3. Waiting for change detection and patch..."
sleep 6

echo ""
echo "4. Reverting change for next time..."
if [[ "$OSTYPE" == "darwin"* ]]; then
  sed -i '' 's/HOT RELOADED!/RAE 2.0/' "$MAIN_RAE"
else
  sed -i 's/HOT RELOADED!/RAE 2.0/' "$MAIN_RAE"
fi

echo "✅ Demo complete."
