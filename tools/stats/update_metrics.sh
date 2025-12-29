#!/usr/bin/env bash
# Collect compiler source metrics (file + line counts) and append to stats/compiler_metrics.jsonl

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC_DIR="$ROOT_DIR/compiler/src"
STATS_DIR="$ROOT_DIR/stats"
METRICS_FILE="$STATS_DIR/compiler_metrics.jsonl"
LATEST_FILE="$STATS_DIR/compiler_metrics_latest.txt"

if [ ! -d "$SRC_DIR" ]; then
  echo "error: compiler/src directory not found at $SRC_DIR" >&2
  exit 1
fi

FILE_COUNT=0
LINE_COUNT=0
while IFS= read -r -d '' file; do
  if [ -f "$file" ]; then
    FILE_COUNT=$((FILE_COUNT + 1))
    lines=$(wc -l < "$file")
    LINE_COUNT=$((LINE_COUNT + lines))
  fi
done < <(find "$SRC_DIR" -type f \( -name '*.c' -o -name '*.h' \) -print0 | sort -z)

if [ "$FILE_COUNT" -eq 0 ]; then
  echo "error: no .c/.h files found under $SRC_DIR" >&2
  exit 1
fi

TIMESTAMP="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
mkdir -p "$STATS_DIR"
JSON_ENTRY=$(printf '{"timestamp":"%s","src_file_count":%d,"src_line_count":%d}' "$TIMESTAMP" "$FILE_COUNT" "$LINE_COUNT")
echo "$JSON_ENTRY" >> "$METRICS_FILE"

cat > "$LATEST_FILE" <<NOTE
Rae compiler metrics
Timestamp: $TIMESTAMP
Source directory: $SRC_DIR
Source files (.c/.h): $FILE_COUNT
Total lines: $LINE_COUNT
NOTE

echo "Recorded compiler metrics: $JSON_ENTRY"
